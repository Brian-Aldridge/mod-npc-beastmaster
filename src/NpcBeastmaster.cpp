/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright
 * information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Affero General Public License as published by the
 * Free Software Foundation; either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but without
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "NpcBeastmaster.h"
#include "Chat.h"
#include "Common.h"
#include "Config.h"
#include "Pet.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "ScriptedCreature.h"
#include "ScriptedGossip.h"
#include "WorldSession.h"
#include <fstream>
#include <locale>
#include <map>
#include <mutex>
#include <regex>
#include <sstream>
#include <sys/stat.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <string_view>

// Helper to get Beastmaster NPC entry from config
static uint32 GetBeastmasterNpcEntry()
{
  return sConfigMgr->GetOption<uint32>("BeastMaster.NpcEntry", 601026);
}

namespace BeastmasterDB
{
  bool TrackTamedPet(Player *player, uint32 creatureEntry,
                     std::string const &petName)
  {
    QueryResult result =
        CharacterDatabase.Query("SELECT 1 FROM beastmaster_tamed_pets WHERE "
                                "owner_guid = {} AND entry = {}",
                                player->GetGUID().GetCounter(), creatureEntry);
    if (result)
      return false; // Already tracked

    CharacterDatabase.Execute("INSERT INTO beastmaster_tamed_pets (owner_guid, "
                              "entry, name) VALUES ({}, {}, '{}')",
                              player->GetGUID().GetCounter(), creatureEntry,
                              petName.c_str());
    return true;
  }
} // namespace BeastmasterDB

namespace
{
  using PetList = std::vector<PetInfo>;

  // Consolidated runtime state singleton to avoid scattered globals.
  struct BeastmasterRuntime
  {
    // Configuration snapshot
    struct Config
    {
      bool hunterOnly = true;
      bool allowExotic = false;
      bool keepPetHappy = false;
      uint32 minLevel = 10;
      uint32 maxLevel = 0;
      bool hunterBeastMasteryRequired = true;
      bool trackTamedPets = false;
      uint32 maxTrackedPets = 20;
      std::set<uint8> allowedRaces;
      std::set<uint8> allowedClasses;
    } config;

    // Pet data
    PetList allPets;
    PetList normalPets;
    PetList exoticPets;
    PetList rarePets;
    PetList rareExoticPets;
    std::set<uint32> rarePetEntries;
    std::set<uint32> rareExoticPetEntries;
    std::unordered_map<uint32, PetInfo> allPetsByEntry;
    std::mutex petsMutex;

    // Caches
    std::unordered_map<uint64, std::set<uint32>> tamedEntriesCache;
    std::mutex tamedEntriesMutex;
    std::unordered_map<uint64, std::vector<std::tuple<uint32, std::string, std::string>>> trackedPetsCache;
    std::mutex trackedPetsCacheMutex;

    // Hunter spell list for granting/removing abilities
    const std::vector<uint32> hunterSpells = {883, 982, 2641, 6991, 48990, 1002, 1462, 6197};

    // Constants (not in enum to allow arithmetic without casts)
    static constexpr uint32 PET_BEASTMASTER_HOWL = 9036;
    static constexpr uint32 PET_SPELL_CALL_PET = 883;
    static constexpr uint32 PET_SPELL_TAME_BEAST = 13481;
    static constexpr uint32 PET_SPELL_BEAST_MASTERY = 53270;
    static constexpr uint32 PET_MAX_HAPPINESS = 1048000;

    // Gossip / action ranges
    struct Gossip
    {
      static constexpr uint32 PageSize = 13; // pets per page (main pet browsing)
      static constexpr uint32 PetsStart = 501;
      static constexpr uint32 ExoticStart = 601;
      static constexpr uint32 RareStart = 701;
      static constexpr uint32 RareExoticStart = 801;
      static constexpr uint32 PetEntryOffset = 901; // actions >= this encode pet entry adoption
      static constexpr uint32 MainMenu = 50;
      static constexpr uint32 RemoveSkills = 80;
      static constexpr uint32 GossipHello = 601026;
      static constexpr uint32 GossipBrowse = 601027;
      static constexpr uint32 TrackedPetsMenu = 1000; // first page = +1 arithmetic
    };

    struct Tracked
    {
      static constexpr uint32 MenuBase = 1000;   // page arithmetic base
      static constexpr uint32 SummonBase = 2000; // Summon action range start
      static constexpr uint32 RenameBase = 3000; // Rename action range start
      static constexpr uint32 DeleteBase = 4000; // Delete action range start
      static constexpr uint32 PageSize = 10;     // tracked pets per page
    };

    // Helpers to interpret action codes
    static bool IsBrowseNormal(uint32 a) { return a >= Gossip::PetsStart && a < Gossip::ExoticStart; }
    static bool IsBrowseExotic(uint32 a) { return a >= Gossip::ExoticStart && a < Gossip::RareStart; }
    static bool IsBrowseRare(uint32 a) { return a >= Gossip::RareStart && a < Gossip::RareExoticStart; }
    static bool IsBrowseRareExotic(uint32 a) { return a >= Gossip::RareExoticStart && a < Gossip::PetEntryOffset; }
    static bool IsAdoptAction(uint32 a) { return a >= Gossip::PetEntryOffset; }
    static bool IsTrackedMenu(uint32 a) { return a >= Gossip::TrackedPetsMenu && a < Tracked::SummonBase; }
    static bool IsTrackedSummon(uint32 a) { return a >= Tracked::SummonBase && a < Tracked::RenameBase; }
    static bool IsTrackedRename(uint32 a) { return a >= Tracked::RenameBase && a < Tracked::DeleteBase; }
    static bool IsTrackedDelete(uint32 a) { return a >= Tracked::DeleteBase && a < Tracked::DeleteBase + 1000; }

    static BeastmasterRuntime &Instance()
    {
      static BeastmasterRuntime inst;
      return inst;
    }
  };
} // namespace

enum BeastmasterEvents
{
  BEASTMASTER_EVENT_EAT = 1
};

enum TrackedPetActions
{
  PET_TRACKED_SUMMON = 2000,
  PET_TRACKED_RENAME = 3000,
  PET_TRACKED_DELETE = 4000,
  PET_TRACKED_PAGE_SIZE = 10
};

enum
{
  PET_TRACKED_RENAME_PROMPT = 5000
};

static std::unordered_set<std::string> sProfanityList;
static time_t sProfanityListMTime = 0;

static time_t GetFileMTime(std::string_view path)
{
  struct stat statbuf;
  if (stat(path.c_str(), &statbuf) == 0)
    return statbuf.st_mtime;
  return 0;
}

static std::set<uint8> ParseAllowedRaces(std::string_view csv)
{
  std::set<uint8> result;
  std::stringstream ss(csv);
  std::string item;
  while (std::getline(ss, item, ','))
  {
    try
    {
      uint8 race = static_cast<uint8>(std::stoul(item));
      if (race > 0)
        result.insert(race);
    }
    catch (...)
    {
    }
  }
  return result;
}

static std::set<uint8> ParseAllowedClasses(std::string_view csv)
{
  std::set<uint8> result;
  std::stringstream ss(csv);
  std::string item;
  while (std::getline(ss, item, ','))
  {
    try
    {
      uint8 cls = static_cast<uint8>(std::stoul(item));
      if (cls > 0)
        result.insert(cls);
    }
    catch (...)
    {
    }
  }
  return result;
}

static void LoadProfanityListIfNeeded()
{
  const std::string path = "modules/mod-npc-beastmaster/conf/profanity.txt";
  time_t mtime = GetFileMTime(path);
  if (mtime == 0)
    return;
  if (mtime == sProfanityListMTime && !sProfanityList.empty())
    return;
  sProfanityList.clear();
  std::ifstream f(path);
  if (!f.is_open())
  {
    LOG_WARN("module", "Beastmaster: Could not open profanity.txt, skipping "
                       "profanity filter.");
    return;
  }
  std::string word;
  while (std::getline(f, word))
  {
    std::transform(word.begin(), word.end(), word.begin(), ::tolower);
    if (!word.empty())
      sProfanityList.insert(word);
  }
  sProfanityListMTime = mtime;
  LOG_INFO("module", "Beastmaster: Loaded {} profane words (mtime={})",
           sProfanityList.size(), long(mtime));
}

static bool IsProfane(std::string_view name)
{
  if (!sConfigMgr->GetOption<bool>("BeastMaster.ProfanityFilter", true))
    return false;
  LoadProfanityListIfNeeded();
  std::string lower = name;
  std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
  for (auto const &bad : sProfanityList)
    if (lower.find(bad) != std::string::npos)
      return true;
  return false;
}

static bool IsValidPetName(std::string_view name)
{
  if (name.size() < 2 || name.size() > 16)
    return false;
  if (std::isspace(name.front()) || std::isspace(name.back()))
    return false;
  static const std::regex allowed("^[A-Za-z][A-Za-z \\-']*[A-Za-z]$");
  return std::regex_match(name, allowed);
}

static std::set<uint32> ParseEntryList(std::string_view csv)
{
  std::set<uint32> result;
  std::stringstream ss(csv);
  std::string item;
  while (std::getline(ss, item, ','))
  {
    try
    {
      result.insert(std::stoul(item));
    }
    catch (...)
    {
    }
  }
  return result;
}

static const PetInfo *FindPetInfo(uint32 entry)
{
  auto &rt = BeastmasterRuntime::Instance();
  std::lock_guard<std::mutex> lock(rt.petsMutex);
  auto it = rt.allPetsByEntry.find(entry);
  return it != rt.allPetsByEntry.end() ? &it->second : nullptr;
}

class BeastmasterBool : public DataMap::Base
{
public:
  explicit BeastmasterBool(bool v) : value(v) {}
  bool value;
};

class BeastmasterUInt32 : public DataMap::Base
{
public:
  explicit BeastmasterUInt32(uint32 v) : value(v) {}
  uint32 value;
};

class BeastmasterPetMap : public DataMap::Base
{
public:
  std::map<uint32, uint32> map;
  BeastmasterPetMap(const std::map<uint32, uint32> &m) : map(m) {}
};

/*static*/ NpcBeastmaster *NpcBeastmaster::instance()
{
  static NpcBeastmaster instance;
  return &instance;
}

void NpcBeastmaster::LoadSystem(bool /*reload = false*/)
{
  auto &rt = BeastmasterRuntime::Instance();
  std::lock_guard<std::mutex> lock(rt.petsMutex);

  // --- Basic schema verification (non-fatal) -----------------------------
  // We don't migrate here, only warn if expected tables/columns missing.
  auto VerifySchema = []()
  {
    struct TableCheck
    {
      const char *name;
      const char *cols;
    };
    // Expected columns expressed as a comma separated list for quick LIKE matching.
    // NOTE: This is a lightweight heuristic, not a strict checksum.
    TableCheck worldTable{"beastmaster_tames", "entry,name,family,rarity"};
    TableCheck charTable{"beastmaster_tamed_pets", "owner_guid,entry,name,date_tamed"};

    // helper lambda to check existence
    auto HasTable = [](const char *table, bool world) -> bool
    {
      QueryResult r = (world ? WorldDatabase.Query("SHOW TABLES LIKE '{}'", table)
                             : CharacterDatabase.Query("SHOW TABLES LIKE '{}'", table));
      return r != nullptr; };
    auto Columns = [](const char *table, bool world) -> std::set<std::string>
    {
      std::set<std::string> out; QueryResult r = (world ? WorldDatabase.Query("SHOW COLUMNS FROM {}", table)
                                                        : CharacterDatabase.Query("SHOW COLUMNS FROM {}", table));
      if (!r) return out; do { Field* f = r->Fetch(); out.insert(f[0].Get<std::string>()); } while (r->NextRow()); return out; };

    // beastmaster_tames (world)
    if (!HasTable(worldTable.name, true))
    {
      LOG_ERROR("module", "Beastmaster: Expected world table '{}' missing. Pets cannot load.", worldTable.name);
    }
    else
    {
      auto cols = Columns(worldTable.name, true);
      std::vector<std::string> missing;
      for (auto &c : {"entry", "name", "family", "rarity"})
        if (!cols.count(c))
          missing.push_back(c);
      if (!missing.empty())
      {
        std::string joined;
        for (size_t i = 0; i < missing.size(); ++i)
        {
          if (i)
            joined += ",";
          joined += missing[i];
        }
        LOG_WARN("module", "Beastmaster: Table '{}' missing columns: {}. Module may misbehave.", worldTable.name, joined);
      }
    }
    // beastmaster_tamed_pets (characters)
    if (!HasTable(charTable.name, false))
    {
      LOG_WARN("module", "Beastmaster: Optional characters table '{}' missing (tracking disabled).", charTable.name);
    }
    else
    {
      auto cols = Columns(charTable.name, false);
      std::vector<std::string> missing;
      for (auto &c : {"owner_guid", "entry", "name", "date_tamed"})
        if (!cols.count(c))
          missing.push_back(c);
      if (!missing.empty())
      {
        std::string joined;
        for (size_t i = 0; i < missing.size(); ++i)
        {
          if (i)
            joined += ",";
          joined += missing[i];
        }
        LOG_WARN("module", "Beastmaster: Table '{}' missing columns: {}. Tracking may fail.", charTable.name, joined);
      }
    }
  }; // VerifySchema

  VerifySchema();

  rt.config.hunterOnly =
      sConfigMgr->GetOption<bool>("BeastMaster.HunterOnly", true);
  rt.config.allowExotic =
      sConfigMgr->GetOption<bool>("BeastMaster.AllowExotic", false);
  rt.config.keepPetHappy =
      sConfigMgr->GetOption<bool>("BeastMaster.KeepPetHappy", false);
  rt.config.minLevel =
      sConfigMgr->GetOption<uint32>("BeastMaster.MinLevel", 10);
  rt.config.maxLevel =
      sConfigMgr->GetOption<uint32>("BeastMaster.MaxLevel", 0);
  rt.config.hunterBeastMasteryRequired = sConfigMgr->GetOption<uint32>(
      "BeastMaster.HunterBeastMasteryRequired", true);
  rt.config.trackTamedPets =
      sConfigMgr->GetOption<bool>("BeastMaster.TrackTamedPets", false);
  rt.config.maxTrackedPets =
      sConfigMgr->GetOption<uint32>("BeastMaster.MaxTrackedPets", 20);
  rt.config.allowedRaces = ParseAllowedRaces(
      sConfigMgr->GetOption<std::string>("BeastMaster.AllowedRaces", "0"));
  rt.config.allowedClasses = ParseAllowedClasses(
      sConfigMgr->GetOption<std::string>("BeastMaster.AllowedClasses", "0"));

  // --- Validation & Normalization ---------------------------------------
  // If hunterOnly is set but AllowedClasses contains other classes, log a warning
  if (rt.config.hunterOnly && !rt.config.allowedClasses.empty() &&
      (rt.config.allowedClasses.size() != 1 ||
       !rt.config.allowedClasses.count(CLASS_HUNTER)))
  {
    LOG_WARN("module",
             "Beastmaster: HunterOnly=1 but AllowedClasses contains non-hunter classes. HunterOnly takes precedence.");
  }

  // Level bounds sanity
  if (rt.config.maxLevel != 0 &&
      rt.config.maxLevel < rt.config.minLevel &&
      rt.config.minLevel != 0)
  {
    LOG_WARN("module",
             "Beastmaster: MaxLevel ({}) is lower than MinLevel ({}). Swapping values.",
             rt.config.maxLevel, rt.config.minLevel);
    std::swap(rt.config.maxLevel, rt.config.minLevel);
  }

  // TrackTamedPets + MaxTrackedPets logic
  if (!rt.config.trackTamedPets &&
      sConfigMgr->GetOption<uint32>("BeastMaster.MaxTrackedPets", 20) == 0)
  {
    LOG_INFO("module",
             "Beastmaster: Tracking disabled; MaxTrackedPets ignored (set to {}).",
             rt.config.maxTrackedPets);
  }

  // Guard against extreme MaxTrackedPets (potential performance issues)
  if (rt.config.trackTamedPets &&
      rt.config.maxTrackedPets > 1000 &&
      rt.config.maxTrackedPets != 0)
  {
    LOG_WARN(
        "module",
        "Beastmaster: MaxTrackedPets={} is very high and may impact performance.",
        rt.config.maxTrackedPets);
  }

  // Warn if both AllowExotic for non-hunters and HunterBeastMasteryRequired are set – clarify behavior.
  if (rt.config.allowExotic &&
      rt.config.hunterBeastMasteryRequired)
  {
    LOG_INFO(
        "module",
        "Beastmaster: AllowExotic=1 allows non-hunters exotic pets regardless of HunterBeastMasteryRequired.");
  }

  rt.rarePetEntries = ParseEntryList(
      sConfigMgr->GetOption<std::string>("BeastMaster.RarePets", ""));
  rt.rareExoticPetEntries = ParseEntryList(
      sConfigMgr->GetOption<std::string>("BeastMaster.RareExoticPets", ""));
  rt.allPets.clear();
  rt.normalPets.clear();
  rt.exoticPets.clear();
  rt.rarePets.clear();
  rt.rareExoticPets.clear();
  rt.allPetsByEntry.clear();

  QueryResult result = WorldDatabase.Query(
      "SELECT entry, name, family, rarity FROM beastmaster_tames");
  if (!result)
  {
    LOG_ERROR(
        "module",
        "Beastmaster: Could not load tames from beastmaster_tames table!");
    return;
  }

  do
  {
    Field *fields = result->Fetch();
    PetInfo info;
    info.entry = fields[0].Get<uint32>();
    info.name = fields[1].Get<std::string>();
    info.family = fields[2].Get<uint32>();
    info.rarity = fields[3].Get<std::string>();

    static const std::set<uint32> TrainerIconFamilies = {
        1, 2, 3, 4, 7, 8, 9, 10, 15, 20, 21, 30, 24, 31, 25, 34, 27};

    if (TrainerIconFamilies.count(info.family))
      info.icon = GOSSIP_ICON_TRAINER;
    else
      info.icon = GOSSIP_ICON_VENDOR;

    rt.allPets.push_back(info);
    rt.allPetsByEntry[info.entry] = info;

    if (rt.rarePetEntries.count(info.entry))
      rt.rarePets.push_back(info);
    else if (rt.rareExoticPetEntries.count(info.entry))
      rt.rareExoticPets.push_back(info);
    else if (info.rarity == "exotic")
      rt.exoticPets.push_back(info);
    else
      rt.normalPets.push_back(info);
  } while (result->NextRow());

  // Post-load logging summary
  LOG_INFO("module", "Beastmaster: Loaded pets - total={}, normal={}, exotic={}, rare={}, rare_exotic={}",
           rt.allPets.size(), rt.normalPets.size(), rt.exoticPets.size(), rt.rarePets.size(), rt.rareExoticPets.size());
  if (rt.allPets.empty())
  {
    LOG_ERROR("module", "Beastmaster: No pets loaded! Check beastmaster_tames table/import.");
  }
}

void NpcBeastmaster::ShowMainMenu(Player *player, Creature *creature)
{
  // Module enable check
  if (!sConfigMgr->GetOption<bool>("BeastMaster.Enable", true))
    return;

  // Safety: if pet lists failed to load (e.g. alternate core fork missing the
  // WORLDHOOK_ON_BEFORE_CONFIG_LOAD timing) attempt a lazy load once.
  auto &rt = BeastmasterRuntime::Instance();
  bool needLoad = false;
  {
    std::lock_guard<std::mutex> lock(rt.petsMutex);
    needLoad = rt.allPets.empty();
  }
  if (needLoad)
  {
    LOG_WARN("module", "Beastmaster: Pet lists empty at ShowMainMenu; performing lazy LoadSystem().");
    sNpcBeastMaster->LoadSystem();
    std::lock_guard<std::mutex> lock(rt.petsMutex);
    if (rt.allPets.empty())
    {
      if (creature)
        creature->Whisper("No pets available (beastmaster_tames table empty?). Contact an administrator.", LANG_UNIVERSAL, player);
      else
        ChatHandler(player->GetSession()).PSendSysMessage("No pets available (beastmaster_tames table empty?). Contact an administrator.");
      return;
    }
  }

  if (rt.config.hunterOnly && player->getClass() != CLASS_HUNTER)
  {
    if (creature)
      creature->Whisper("I am sorry, but pets are for hunters only.",
                        LANG_UNIVERSAL, player);
    else
      ChatHandler(player->GetSession())
          .PSendSysMessage("I am sorry, but pets are for hunters only.");
    return;
  }

  if (!rt.config.allowedClasses.empty() &&
      rt.config.allowedClasses.find(player->getClass()) ==
          rt.config.allowedClasses.end())
  {
    if (creature)
      creature->Whisper("Your class is not allowed to adopt pets.",
                        LANG_UNIVERSAL, player);
    else
      ChatHandler(player->GetSession())
          .PSendSysMessage("Your class is not allowed to adopt pets.");
    return;
  }

  if (!rt.config.allowedRaces.empty() &&
      rt.config.allowedRaces.find(player->getRace()) ==
          rt.config.allowedRaces.end())
  {
    if (creature)
      creature->Whisper("Your race is not allowed to adopt pets.",
                        LANG_UNIVERSAL, player);
    else
      ChatHandler(player->GetSession())
          .PSendSysMessage("Your race is not allowed to adopt pets.");
    return;
  }

  if (player->GetLevel() < rt.config.minLevel &&
      rt.config.minLevel != 0)
  {
    std::string messageExperience = Acore::StringFormat(
        "Sorry {}, but you must reach level {} before adopting a pet.",
        player->GetName(), rt.config.minLevel);
    if (creature)
      creature->Whisper(messageExperience.c_str(), LANG_UNIVERSAL, player);
    else
      ChatHandler(player->GetSession())
          .PSendSysMessage("%s", messageExperience.c_str());
    return;
  }

  if (rt.config.maxLevel != 0 &&
      player->GetLevel() > rt.config.maxLevel)
  {
    std::string message = Acore::StringFormat(
        "Sorry {}, but you must be level {} or lower to adopt a pet.",
        player->GetName(), rt.config.maxLevel);
    if (creature)
      creature->Whisper(message.c_str(), LANG_UNIVERSAL, player);
    else
      ChatHandler(player->GetSession()).PSendSysMessage("%s", message.c_str());
    return;
  }

  ClearGossipMenuFor(player);

  AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Browse Pets",
                   GOSSIP_SENDER_MAIN, BeastmasterRuntime::Gossip::PetsStart);
  AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Browse Rare Pets",
                   GOSSIP_SENDER_MAIN, BeastmasterRuntime::Gossip::RareStart);

  if (rt.config.allowExotic ||
      player->HasSpell(BeastmasterRuntime::PET_SPELL_BEAST_MASTERY) ||
      player->HasTalent(BeastmasterRuntime::PET_SPELL_BEAST_MASTERY, player->GetActiveSpec()))
  {
    if (player->getClass() != CLASS_HUNTER)
    {
      AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Browse Exotic Pets",
                       GOSSIP_SENDER_MAIN, BeastmasterRuntime::Gossip::ExoticStart);
      AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Browse Rare Exotic Pets",
                       GOSSIP_SENDER_MAIN, BeastmasterRuntime::Gossip::RareExoticStart);
    }
    else if (!rt.config.hunterBeastMasteryRequired ||
             player->HasTalent(BeastmasterRuntime::PET_SPELL_BEAST_MASTERY,
                               player->GetActiveSpec()))
    {
      AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Browse Exotic Pets",
                       GOSSIP_SENDER_MAIN, BeastmasterRuntime::Gossip::ExoticStart);
      AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Browse Rare Exotic Pets",
                       GOSSIP_SENDER_MAIN, BeastmasterRuntime::Gossip::RareExoticStart);
    }
  }

  if (player->getClass() != CLASS_HUNTER &&
      player->HasSpell(BeastmasterRuntime::PET_SPELL_CALL_PET))
    AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Unlearn Hunter Abilities",
                     GOSSIP_SENDER_MAIN, BeastmasterRuntime::Gossip::RemoveSkills);

  if (rt.config.trackTamedPets)
    AddGossipItemFor(player, GOSSIP_ICON_CHAT, "My Tamed Pets",
                     GOSSIP_SENDER_MAIN, BeastmasterRuntime::Gossip::TrackedPetsMenu);

  if (player->getClass() == CLASS_HUNTER)
    AddGossipItemFor(player, GOSSIP_ICON_TAXI, "Visit Stable",
                     GOSSIP_SENDER_MAIN, GOSSIP_OPTION_STABLEPET);

  AddGossipItemFor(player, GOSSIP_ICON_MONEY_BAG, "Buy Pet Food",
                   GOSSIP_SENDER_MAIN, GOSSIP_OPTION_VENDOR);

  if (creature)
    SendGossipMenuFor(player, BeastmasterRuntime::Gossip::GossipHello, creature->GetGUID());
  else
    SendGossipMenuFor(player, BeastmasterRuntime::Gossip::GossipHello, ObjectGuid::Empty);

  player->PlayDirectSound(BeastmasterRuntime::PET_BEASTMASTER_HOWL);
}

void NpcBeastmaster::GossipSelect(Player *player, Creature *creature,
                                  uint32 action)
{
  if (!sConfigMgr->GetOption<bool>("BeastMaster.Enable", true))
    return;

  auto &rt = BeastmasterRuntime::Instance();
  // Lazy load safeguard for forks where initial LoadSystem hook may not fire.
  {
    bool emptyNow = false;
    {
      std::lock_guard<std::mutex> lock(rt.petsMutex);
      emptyNow = rt.allPets.empty();
    }
    if (emptyNow)
      sNpcBeastMaster->LoadSystem();
  }

  ClearGossipMenuFor(player);

  if (action == BeastmasterRuntime::Gossip::MainMenu)
  {
    ShowMainMenu(player, creature);
  }
  else if (BeastmasterRuntime::IsBrowseNormal(action))
  {
    AddGossipItemFor(player, GOSSIP_ICON_TALK, "Back..", GOSSIP_SENDER_MAIN,
                     BeastmasterRuntime::Gossip::MainMenu);
    int page = action - BeastmasterRuntime::Gossip::PetsStart + 1;
    int maxPage = rt.normalPets.size() / BeastmasterRuntime::Gossip::PageSize +
                  (rt.normalPets.size() % BeastmasterRuntime::Gossip::PageSize != 0);

    if (page > 1)
      AddGossipItemFor(player, GOSSIP_ICON_INTERACT_1, "Previous..",
                       GOSSIP_SENDER_MAIN, BeastmasterRuntime::Gossip::PetsStart + page - 2);

    if (page < maxPage)
      AddGossipItemFor(player, GOSSIP_ICON_INTERACT_1, "Next..",
                       GOSSIP_SENDER_MAIN, BeastmasterRuntime::Gossip::PetsStart + page);

    AddPetsToGossip(player, rt.normalPets, page);
    SendGossipMenuFor(player, BeastmasterRuntime::Gossip::GossipBrowse, creature->GetGUID());
  }
  else if (BeastmasterRuntime::IsBrowseExotic(action))
  {
    if (!(player->HasSpell(BeastmasterRuntime::PET_SPELL_BEAST_MASTERY) ||
          player->HasTalent(BeastmasterRuntime::PET_SPELL_BEAST_MASTERY,
                            player->GetActiveSpec())))
    {
      player->addSpell(BeastmasterRuntime::PET_SPELL_BEAST_MASTERY, SPEC_MASK_ALL, false);
      std::ostringstream messageLearn;
      messageLearn << "I have taught you the art of Beast Mastery, "
                   << player->GetName() << ".";
      creature->Whisper(messageLearn.str().c_str(), LANG_UNIVERSAL, player);
    }

    AddGossipItemFor(player, GOSSIP_ICON_TALK, "Back..", GOSSIP_SENDER_MAIN,
                     BeastmasterRuntime::Gossip::MainMenu);
    int page = action - BeastmasterRuntime::Gossip::ExoticStart + 1;
    int maxPage = rt.exoticPets.size() / BeastmasterRuntime::Gossip::PageSize +
                  (rt.exoticPets.size() % BeastmasterRuntime::Gossip::PageSize != 0);

    if (page > 1)
      AddGossipItemFor(player, GOSSIP_ICON_INTERACT_1, "Previous..",
                       GOSSIP_SENDER_MAIN, BeastmasterRuntime::Gossip::ExoticStart + page - 2);

    if (page < maxPage)
      AddGossipItemFor(player, GOSSIP_ICON_INTERACT_1, "Next..",
                       GOSSIP_SENDER_MAIN, BeastmasterRuntime::Gossip::ExoticStart + page);

    AddPetsToGossip(player, rt.exoticPets, page);
    SendGossipMenuFor(player, BeastmasterRuntime::Gossip::GossipBrowse, creature->GetGUID());
  }
  else if (BeastmasterRuntime::IsBrowseRare(action))
  {
    AddGossipItemFor(player, GOSSIP_ICON_TALK, "Back..", GOSSIP_SENDER_MAIN,
                     BeastmasterRuntime::Gossip::MainMenu);
    int page = action - BeastmasterRuntime::Gossip::RareStart + 1;
    int maxPage = rt.rarePets.size() / BeastmasterRuntime::Gossip::PageSize +
                  (rt.rarePets.size() % BeastmasterRuntime::Gossip::PageSize != 0);

    if (page > 1)
      AddGossipItemFor(player, GOSSIP_ICON_INTERACT_1, "Previous..",
                       GOSSIP_SENDER_MAIN, BeastmasterRuntime::Gossip::RareStart + page - 2);

    if (page < maxPage)
      AddGossipItemFor(player, GOSSIP_ICON_INTERACT_1, "Next..",
                       GOSSIP_SENDER_MAIN, BeastmasterRuntime::Gossip::RareStart + page);

    AddPetsToGossip(player, rt.rarePets, page);
    SendGossipMenuFor(player, BeastmasterRuntime::Gossip::GossipBrowse, creature->GetGUID());
  }
  else if (BeastmasterRuntime::IsBrowseRareExotic(action))
  {
    if (!(player->HasSpell(BeastmasterRuntime::PET_SPELL_BEAST_MASTERY) ||
          player->HasTalent(BeastmasterRuntime::PET_SPELL_BEAST_MASTERY,
                            player->GetActiveSpec())))
    {
      player->addSpell(BeastmasterRuntime::PET_SPELL_BEAST_MASTERY, SPEC_MASK_ALL, false);
      std::ostringstream messageLearn;
      messageLearn << "I have taught you the art of Beast Mastery, "
                   << player->GetName() << ".";
      creature->Whisper(messageLearn.str().c_str(), LANG_UNIVERSAL, player);
    }

    AddGossipItemFor(player, GOSSIP_ICON_TALK, "Back..", GOSSIP_SENDER_MAIN,
                     BeastmasterRuntime::Gossip::MainMenu);
    int page = action - BeastmasterRuntime::Gossip::RareExoticStart + 1;
    int maxPage = rt.rareExoticPets.size() / BeastmasterRuntime::Gossip::PageSize +
                  (rt.rareExoticPets.size() % BeastmasterRuntime::Gossip::PageSize != 0);

    if (page > 1)
      AddGossipItemFor(player, GOSSIP_ICON_INTERACT_1, "Previous..",
                       GOSSIP_SENDER_MAIN, BeastmasterRuntime::Gossip::RareExoticStart + page - 2);

    if (page < maxPage)
      AddGossipItemFor(player, GOSSIP_ICON_INTERACT_1, "Next..",
                       GOSSIP_SENDER_MAIN, BeastmasterRuntime::Gossip::RareExoticStart + page);

    AddPetsToGossip(player, rt.rareExoticPets, page);
    SendGossipMenuFor(player, BeastmasterRuntime::Gossip::GossipBrowse, creature->GetGUID());
  }
  else if (action == BeastmasterRuntime::Gossip::RemoveSkills)
  {
    for (auto spell : rt.hunterSpells)
      player->removeSpell(spell, SPEC_MASK_ALL, false);

    player->removeSpell(BeastmasterRuntime::PET_SPELL_BEAST_MASTERY, SPEC_MASK_ALL, false);
    CloseGossipMenuFor(player);
  }
  else if (action == GOSSIP_OPTION_STABLEPET)
  {
    player->GetSession()->SendStablePet(creature->GetGUID());
  }
  else if (action == GOSSIP_OPTION_VENDOR)
  {
    player->GetSession()->SendListInventory(creature->GetGUID());
  }
  else if (BeastmasterRuntime::IsTrackedMenu(action))
  {
    uint32 page = action - BeastmasterRuntime::Gossip::TrackedPetsMenu + 1;
    ShowTrackedPetsMenu(player, creature, page);
    return;
  }
  else if (BeastmasterRuntime::IsTrackedSummon(action))
  {
    uint32 idx = action - BeastmasterRuntime::Tracked::SummonBase;
    auto *petMapWrap =
        player->CustomData.Get<BeastmasterPetMap>("BeastmasterMenuPetMap");
    if (!petMapWrap || petMapWrap->map.find(idx) == petMapWrap->map.end())
      return;
    uint32 entry = petMapWrap->map[idx];
    if (player->IsExistPet())
    {
      creature->Whisper("First you must abandon or stable your current pet!",
                        LANG_UNIVERSAL, player);
      CloseGossipMenuFor(player);
      return;
    }
    else
    {
      Pet *pet = player->CreatePet(entry, BeastmasterRuntime::PET_SPELL_CALL_PET);
      if (pet)
      {
        QueryResult nameResult =
            CharacterDatabase.Query("SELECT name FROM beastmaster_tamed_pets "
                                    "WHERE owner_guid = {} AND entry = {}",
                                    player->GetGUID().GetCounter(), entry);
        if (nameResult)
        {
          std::string customName = (*nameResult)[0].Get<std::string>();
          pet->SetName(customName);
        }
        pet->SetPower(POWER_HAPPINESS, BeastmasterRuntime::PET_MAX_HAPPINESS);
        creature->Whisper("Your tracked pet has been summoned!", LANG_UNIVERSAL,
                          player);
      }
      else
      {
        creature->Whisper("Failed to summon pet.", LANG_UNIVERSAL, player);
      }
    }
    CloseGossipMenuFor(player);
    return;
  }
  else if (BeastmasterRuntime::IsTrackedRename(action))
  {
    uint32 idx = action - BeastmasterRuntime::Tracked::RenameBase;
    auto *petMapWrap =
        player->CustomData.Get<BeastmasterPetMap>("BeastmasterMenuPetMap");
    if (!petMapWrap || petMapWrap->map.find(idx) == petMapWrap->map.end())
      return;
    uint32 entry = petMapWrap->map[idx];
    player->CustomData.Set("BeastmasterRenamePetEntry",
                           new BeastmasterUInt32(entry));
    player->CustomData.Set("BeastmasterExpectRename",
                           new BeastmasterBool(true));
    ChatHandler(player->GetSession())
        .PSendSysMessage("To rename your pet, type: .petname rename <newname> "
                         "in chat. To cancel, type: .petname cancel");
    if (creature)
      creature->Whisper(
          "To rename your pet, type: .petname rename <newname> in chat. "
          "To cancel, type: .petname cancel",
          LANG_UNIVERSAL, player);
    CloseGossipMenuFor(player);
    return;
  }
  else if (BeastmasterRuntime::IsTrackedDelete(action))
  {
    uint32 idx = action - BeastmasterRuntime::Tracked::DeleteBase;
    auto *petMapWrap =
        player->CustomData.Get<BeastmasterPetMap>("BeastmasterMenuPetMap");
    if (!petMapWrap || petMapWrap->map.find(idx) == petMapWrap->map.end())
      return;
    uint32 entry = petMapWrap->map[idx];

    CharacterDatabase.Execute("DELETE FROM beastmaster_tamed_pets WHERE "
                              "owner_guid = {} AND entry = {}",
                              player->GetGUID().GetCounter(), entry);

    sNpcBeastMaster->ClearTrackedPetsCache(player);
    if (rt.config.trackTamedPets)
    {
      std::lock_guard<std::mutex> lock(rt.tamedEntriesMutex);
      auto it = rt.tamedEntriesCache.find(player->GetGUID().GetRawValue());
      if (it != rt.tamedEntriesCache.end())
        it->second.erase(entry);
    }

    ChatHandler(player->GetSession())
        .PSendSysMessage("Tracked pet deleted (entry {}).", entry);
    LOG_INFO("module", "Beastmaster: Player {} deleted tracked pet (entry {}).",
             player->GetGUID().GetCounter(), entry);

    uint32 totalPets = 0;
    QueryResult result = CharacterDatabase.Query(
        "SELECT COUNT(*) FROM beastmaster_tamed_pets WHERE owner_guid = {}",
        player->GetGUID().GetCounter());
    if (result)
      totalPets = (*result)[0].Get<uint32>();

    uint32 page = (idx / BeastmasterRuntime::Tracked::PageSize) + 1;
    uint32 maxPage =
        (totalPets + BeastmasterRuntime::Tracked::PageSize - 1) / BeastmasterRuntime::Tracked::PageSize;
    if (page > maxPage && maxPage > 0)
      page = maxPage;
    if (page == 0)
      page = 1;

    ShowTrackedPetsMenu(player, creature, page);
    return;
  }

  if (BeastmasterRuntime::IsAdoptAction(action))
    CreatePet(player, creature, action);
}

void NpcBeastmaster::CreatePet(Player *player, Creature *creature,
                               uint32 action)
{
  if (!sConfigMgr->GetOption<bool>("BeastMaster.Enable", true))
    return;

  auto &rt = BeastmasterRuntime::Instance();
  uint32 petEntry = action - BeastmasterRuntime::Gossip::PetEntryOffset;
  const PetInfo *info = FindPetInfo(petEntry);

  if (player->IsExistPet())
  {
    creature->Whisper("First you must abandon or stable your current pet!",
                      LANG_UNIVERSAL, player);
    CloseGossipMenuFor(player);
    return;
  }

  if (info && info->rarity == "exotic" && player->getClass() != CLASS_HUNTER &&
      !rt.config.allowExotic)
  {
    creature->Whisper("Only hunters can adopt exotic pets.", LANG_UNIVERSAL,
                      player);
    CloseGossipMenuFor(player);
    return;
  }

  if (info && info->rarity == "exotic" && player->getClass() == CLASS_HUNTER &&
      rt.config.hunterBeastMasteryRequired)
  {
    if (!player->HasTalent(BeastmasterRuntime::PET_SPELL_BEAST_MASTERY, player->GetActiveSpec()))
    {
      creature->Whisper(
          "You need the Beast Mastery talent to adopt exotic pets.",
          LANG_UNIVERSAL, player);
      CloseGossipMenuFor(player);
      return;
    }
  }

  // Enforce max tracked pets if enabled
  if (rt.config.trackTamedPets &&
      rt.config.maxTrackedPets > 0)
  {
    QueryResult result = CharacterDatabase.Query(
        "SELECT COUNT(*) FROM beastmaster_tamed_pets WHERE owner_guid = {}",
        player->GetGUID().GetCounter());
    uint32 count = result ? (*result)[0].Get<uint32>() : 0;
    if (count >= rt.config.maxTrackedPets)
    {
      creature->Whisper("You have reached the maximum number of tracked pets.",
                        LANG_UNIVERSAL, player);
      CloseGossipMenuFor(player);
      return;
    }
  }

  Pet *pet = player->CreatePet(petEntry, player->getClass() == CLASS_HUNTER
                                             ? BeastmasterRuntime::PET_SPELL_TAME_BEAST
                                             : BeastmasterRuntime::PET_SPELL_CALL_PET);
  if (!pet)
  {
    creature->Whisper("First you must abandon or stable your current pet!",
                      LANG_UNIVERSAL, player);
    return;
  }

  if (rt.config.trackTamedPets)
  {
    if (BeastmasterDB::TrackTamedPet(player, petEntry, pet->GetName()))
    {
      std::lock_guard<std::mutex> lock(rt.tamedEntriesMutex);
      rt.tamedEntriesCache[player->GetGUID().GetRawValue()].insert(petEntry);
    }
  }

  pet->SetPower(POWER_HAPPINESS, BeastmasterRuntime::PET_MAX_HAPPINESS);

  if (player->getClass() != CLASS_HUNTER)
  {
    if (!player->HasSpell(BeastmasterRuntime::PET_SPELL_CALL_PET))
    {
      for (auto const &spell : rt.hunterSpells)
        if (!player->HasSpell(spell))
          player->learnSpell(spell);
    }
  }

  std::string messageAdopt =
      Acore::StringFormat("A fine choice {}! Take good care of your {} and you "
                          "will never face your enemies alone.",
                          player->GetName(), pet->GetName());
  creature->Whisper(messageAdopt.c_str(), LANG_UNIVERSAL, player);
  CloseGossipMenuFor(player);
}

void NpcBeastmaster::AddPetsToGossip(Player *player,
                                     std::vector<PetInfo> const &pets,
                                     uint32 page)
{
  auto &rt = BeastmasterRuntime::Instance();
  const std::set<uint32> *tamedPtr = nullptr;
  std::set<uint32> snapshot; // temporary if we need to query
  uint64 guid = player->GetGUID().GetRawValue();
  if (rt.config.trackTamedPets)
  {
    {
      std::lock_guard<std::mutex> lock(rt.tamedEntriesMutex);
      auto it = rt.tamedEntriesCache.find(guid);
      if (it != rt.tamedEntriesCache.end())
        tamedPtr = &it->second;
    }
    if (!tamedPtr)
    {
      QueryResult result = CharacterDatabase.Query(
          "SELECT entry FROM beastmaster_tamed_pets WHERE owner_guid = {}",
          player->GetGUID().GetCounter());
      if (result)
      {
        do
        {
          Field *fields = result->Fetch();
          snapshot.insert(fields[0].Get<uint32>());
        } while (result->NextRow());
      }
      {
        std::lock_guard<std::mutex> lock(rt.tamedEntriesMutex);
        auto &ref = rt.tamedEntriesCache[guid];
        ref = std::move(snapshot);
        tamedPtr = &ref;
      }
    }
  }
  static const std::set<uint32> emptySet;
  const std::set<uint32> &tamedEntries = tamedPtr ? *tamedPtr : emptySet;

  std::lock_guard<std::mutex> lock(rt.petsMutex);

  uint32 count = 1;
  for (const auto &pet : pets)
  {
    if (count > (page - 1) * BeastmasterRuntime::Gossip::PageSize && count <= page * BeastmasterRuntime::Gossip::PageSize)
    {
      if (tamedEntries.count(pet.entry))
      {
        AddGossipItemFor(player, GOSSIP_ICON_CHAT,
                         pet.name + " (Already Tamed)", GOSSIP_SENDER_MAIN,
                         0); // 0 = no action
      }
      else
      {
        AddGossipItemFor(player, pet.icon, pet.name, GOSSIP_SENDER_MAIN,
                         pet.entry + BeastmasterRuntime::Gossip::PetEntryOffset);
      }
    }
    count++;
  }
}

void NpcBeastmaster::ClearTrackedPetsCache(Player *player)
{
  auto &rt = BeastmasterRuntime::Instance();
  std::lock_guard<std::mutex> lock(rt.trackedPetsCacheMutex);
  rt.trackedPetsCache.erase(player->GetGUID().GetRawValue());
  player->CustomData.Erase("BeastmasterMenuPetMap");
}

void NpcBeastmaster::ShowTrackedPetsMenu(Player *player, Creature *creature,
                                         uint32 page /*= 1*/)
{
  ClearGossipMenuFor(player);

  auto &rt = BeastmasterRuntime::Instance();
  uint64 guid = player->GetGUID().GetRawValue();
  std::vector<std::tuple<uint32, std::string, std::string>> *trackedPetsPtr = nullptr;
  {
    std::lock_guard<std::mutex> lock(rt.trackedPetsCacheMutex);
    auto it = rt.trackedPetsCache.find(guid);
    if (it != rt.trackedPetsCache.end())
      trackedPetsPtr = &it->second;
  }

  if (!trackedPetsPtr)
  {
    if (rt.config.trackTamedPets)
    {
      std::vector<std::tuple<uint32, std::string, std::string>> trackedPets;
      QueryResult result = CharacterDatabase.Query(
          "SELECT entry, name, date_tamed FROM beastmaster_tamed_pets WHERE "
          "owner_guid = {} ORDER BY date_tamed DESC",
          player->GetGUID().GetCounter());

      if (result)
      {
        do
        {
          Field *fields = result->Fetch();
          uint32 entry = fields[0].Get<uint32>();
          std::string name = fields[1].Get<std::string>();
          std::string date = fields[2].Get<std::string>();
          trackedPets.emplace_back(entry, name, date);
        } while (result->NextRow());
      }
      {
        std::lock_guard<std::mutex> lock(rt.trackedPetsCacheMutex);
        rt.trackedPetsCache[guid] = std::move(trackedPets);
        trackedPetsPtr = &rt.trackedPetsCache[guid];
      }
    }
  }

  const auto &trackedPets = *trackedPetsPtr;
  uint32 total = trackedPets.size();
  uint32 offset = (page - 1) * BeastmasterRuntime::Tracked::PageSize;
  uint32 shown = 0;

  std::map<uint32, uint32> menuPetIndexToEntry;

  // Build the menu for this page
  for (uint32 i = offset; i < total && shown < BeastmasterRuntime::Tracked::PageSize;
       ++i, ++shown)
  {
    const auto &petTuple = trackedPets[i];
    uint32 entry = std::get<0>(petTuple);
    const std::string &name = std::get<1>(petTuple);
    const PetInfo *info = FindPetInfo(entry);

    std::string label;
    if (info)
      label =
          Acore::StringFormat("{} [{}, {}]", name, info->name, info->rarity);
    else
      label = name;

    // Use shown as the unique index for this page
    uint32 idx = shown;
    menuPetIndexToEntry[idx] = entry;

    AddGossipItemFor(player, GOSSIP_ICON_TAXI, "Summon: " + label,
                     GOSSIP_SENDER_MAIN, BeastmasterRuntime::Tracked::SummonBase + idx);
    AddGossipItemFor(player, GOSSIP_ICON_TRAINER, "Rename: " + label,
                     GOSSIP_SENDER_MAIN, BeastmasterRuntime::Tracked::RenameBase + idx);
    AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Delete: " + label,
                     GOSSIP_SENDER_MAIN, BeastmasterRuntime::Tracked::DeleteBase + idx);
  }
  // Store the mapping for this menu page
  player->CustomData.Set("BeastmasterMenuPetMap",
                         new BeastmasterPetMap(menuPetIndexToEntry));

  // Send the menu to the player
  if (creature)
    SendGossipMenuFor(player, BeastmasterRuntime::Gossip::GossipBrowse, creature);
  else
    SendGossipMenuFor(player, BeastmasterRuntime::Gossip::GossipBrowse, ObjectGuid::Empty);
}

void NpcBeastmaster::PlayerUpdate(Player *player)
{
  auto &rt = BeastmasterRuntime::Instance();
  if (rt.config.keepPetHappy && player->GetPet())
  {
    Pet *pet = player->GetPet();
    if (pet->getPetType() == HUNTER_PET)
      pet->SetPower(POWER_HAPPINESS, BeastmasterRuntime::PET_MAX_HAPPINESS);
  }
}

// Chat handler to process the rename and delete confirmations
class BeastMaster_CreatureScript : public CreatureScript
{
public:
  BeastMaster_CreatureScript() : CreatureScript("BeastMaster") {}

  bool OnGossipHello(Player *player, Creature *creature) override
  {
    sNpcBeastMaster->ShowMainMenu(player, creature);
    return true;
  }

  bool OnGossipSelect(Player *player, Creature *creature, uint32 /*sender*/,
                      uint32 action) override
  {
    sNpcBeastMaster->GossipSelect(player, creature, action);
    return true;
  }

  struct beastmasterAI : public ScriptedAI
  {
    beastmasterAI(Creature *creature) : ScriptedAI(creature) {}

    void Reset() override
    {
      events.ScheduleEvent(BEASTMASTER_EVENT_EAT, urand(30000, 90000));
    }

    void UpdateAI(uint32 diff) override
    {
      events.Update(diff);

      switch (events.ExecuteEvent())
      {
      case BEASTMASTER_EVENT_EAT:
        me->HandleEmoteCommand(EMOTE_ONESHOT_EAT_NO_SHEATHE);
        events.ScheduleEvent(BEASTMASTER_EVENT_EAT, urand(30000, 90000));
        break;
      }
    }

  private:
    EventMap events;
  };

  CreatureAI *GetAI(Creature *creature) const override
  {
    return new beastmasterAI(creature);
  }
};

class BeastMaster_WorldScript : public WorldScript
{
public:
  BeastMaster_WorldScript()
      : WorldScript("BeastMaster_WorldScript",
                    {WORLDHOOK_ON_BEFORE_CONFIG_LOAD}) {}

  void OnBeforeConfigLoad(bool /*reload*/) override
  {
    sNpcBeastMaster->LoadSystem();
  }
};

class BeastMaster_PlayerScript : public PlayerScript
{
public:
  BeastMaster_PlayerScript()
      : PlayerScript("BeastMaster_PlayerScript",
                     {PLAYERHOOK_ON_BEFORE_UPDATE,
                      PLAYERHOOK_ON_BEFORE_LOAD_PET_FROM_DB,
                      PLAYERHOOK_ON_BEFORE_GUARDIAN_INIT_STATS_FOR_LEVEL}) {}

  void OnPlayerBeforeUpdate(Player *player, uint32 /*p_time*/) override
  {
    sNpcBeastMaster->PlayerUpdate(player);
  }

  void OnPlayerBeforeLoadPetFromDB(Player * /*player*/, uint32 & /*petentry*/,
                                   uint32 & /*petnumber*/, bool & /*current*/,
                                   bool &forceLoadFromDB) override
  {
    forceLoadFromDB = true;
  }

  void OnPlayerBeforeGuardianInitStatsForLevel(Player * /*player*/,
                                               Guardian * /*guardian*/,
                                               CreatureTemplate const *cinfo,
                                               PetType &petType) override
  {
    if (cinfo->IsTameable(true))
      petType = HUNTER_PET;
  }
};

// Rename and cancel commands for pet renaming
class BeastMaster_CommandScript : public CommandScript
{
public:
  BeastMaster_CommandScript() : CommandScript("BeastMaster") {}

  Acore::ChatCommands::ChatCommandTable GetCommands() const override;

  // Remove old handler declarations:
  // static bool HandlePetnameCommand(ChatHandler *handler, std::string_view
  // args); static bool HandleCancelCommand(ChatHandler *handler,
  // std::string_view args);

  // Use only the new handlers:
  static bool HandlePetnameRenameCommand(ChatHandler *handler,
                                         std::string_view args);
  static bool HandlePetnameCancelCommand(ChatHandler *handler,
                                         std::string_view args);
  static bool HandleBeastmasterCommand(ChatHandler *handler, const char *args);
};

// Define GetCommands outside the class body
Acore::ChatCommands::ChatCommandTable
BeastMaster_CommandScript::GetCommands() const
{
  using namespace Acore::ChatCommands;
  static ChatCommandTable petnameTable = {
      {"rename", HandlePetnameRenameCommand, SEC_PLAYER, Console::No},
      {"cancel", HandlePetnameCancelCommand, SEC_PLAYER, Console::No}};
  static ChatCommandTable beastmasterSub = {
      {"reload", [](ChatHandler *handler, const char * /*args*/)
       {
          if (handler->GetSession()->GetSecurity() < SEC_GAMEMASTER && !handler->IsConsole()) {
            handler->PSendSysMessage("Insufficient privileges.");
            return true;
          }
          sNpcBeastMaster->LoadSystem(true);
          handler->PSendSysMessage("Beastmaster configuration & pet lists reloaded.");
          LOG_INFO("module", "Beastmaster: Reload triggered via .beastmaster reload");
          return true; }, SEC_PLAYER, Console::Yes}};
  return {{"beastmaster", beastmasterSub},
          {"beastmaster", HandleBeastmasterCommand, SEC_PLAYER, Console::No},
          {"petname", petnameTable}};
}

// Implement the new handlers:
bool BeastMaster_CommandScript::HandlePetnameRenameCommand(
    ChatHandler *handler, std::string_view args)
{
  Player *player = handler->GetSession()->GetPlayer();
  auto *expectRename =
      player->CustomData.Get<BeastmasterBool>("BeastmasterExpectRename");
  auto *renameEntry =
      player->CustomData.Get<BeastmasterUInt32>("BeastmasterRenamePetEntry");
  if (!expectRename || !expectRename->value || !renameEntry)
  {
    handler->PSendSysMessage("You are not renaming a pet right now. Use the "
                             "Beastmaster NPC to start renaming.");
    return true;
  }

  std::string newName(args);
  while (!newName.empty() && std::isspace(newName.front()))
    newName.erase(newName.begin());
  while (!newName.empty() && std::isspace(newName.back()))
    newName.pop_back();

  if (newName.empty())
  {
    handler->PSendSysMessage("Usage: .petname rename <newname>");
    return true;
  }

  if (!IsValidPetName(newName) || IsProfane(newName))
  {
    handler->PSendSysMessage("Invalid or profane pet name. Please try again "
                             "with .petname rename <newname>.");
    return true;
  }

  CharacterDatabase.Execute("UPDATE beastmaster_tamed_pets SET name = '{}' "
                            "WHERE owner_guid = {} AND entry = {}",
                            newName, player->GetGUID().GetCounter(),
                            renameEntry->value);

  player->CustomData.Erase("BeastmasterExpectRename");
  player->CustomData.Erase("BeastmasterRenamePetEntry");

  handler->PSendSysMessage("Pet renamed to '{}'.", newName);
  sNpcBeastMaster->ClearTrackedPetsCache(player);
  return true;
}

bool BeastMaster_CommandScript::HandlePetnameCancelCommand(
    ChatHandler *handler, std::string_view /*args*/)
{
  Player *player = handler->GetSession()->GetPlayer();
  auto *expectRename =
      player->CustomData.Get<BeastmasterBool>("BeastmasterExpectRename");
  if (!expectRename || !expectRename->value)
  {
    handler->PSendSysMessage("You are not renaming a pet right now.");
    return true;
  }
  player->CustomData.Erase("BeastmasterExpectRename");
  player->CustomData.Erase("BeastmasterRenamePetEntry");
  handler->PSendSysMessage("Pet renaming cancelled.");
  return true;
}

bool BeastMaster_CommandScript::HandleBeastmasterCommand(
    ChatHandler *handler, const char * /*args*/)
{
  Player *player = handler->GetSession()->GetPlayer();
  if (!player)
    return false;

  float x = player->GetPositionX();
  float y = player->GetPositionY();
  float z = player->GetPositionZ();
  float o = player->GetOrientation();

  static std::unordered_map<uint64, time_t> lastSummonTime;
  uint64 guid = player->GetGUID().GetRawValue();
  time_t now = time(nullptr);
  uint32 cooldown =
      sConfigMgr->GetOption<uint32>("BeastMaster.SummonCooldown", 120);
  if (lastSummonTime.count(guid) && now - lastSummonTime[guid] < cooldown)
  {
    handler->PSendSysMessage(
        "You must wait {} seconds before summoning the Beastmaster again.",
        cooldown - (now - lastSummonTime[guid]));
    return true;
  }
  lastSummonTime[guid] = now;

  Creature *npc = player->SummonCreature(GetBeastmasterNpcEntry(), x, y, z, o,
                                         TEMPSUMMON_TIMED_DESPAWN_OUT_OF_COMBAT,
                                         2 * MINUTE * IN_MILLISECONDS);

  if (npc)
  {
    handler->PSendSysMessage(
        "The Beastmaster has arrived and will remain for 2 minutes.");
  }
  else
  {
    handler->PSendSysMessage(
        "Failed to summon the Beastmaster. Please contact an admin.");
  }
  return true;
}

class BeastmasterLoginNotice_PlayerScript : public PlayerScript
{
public:
  BeastmasterLoginNotice_PlayerScript()
      : PlayerScript("BeastmasterLoginNotice_PlayerScript") {}

  void OnLogin(Player *player)
  {
    if (!sConfigMgr->GetOption<bool>("BeastMaster.ShowLoginNotice", true))
      return;

    if (!sConfigMgr->GetOption<bool>("BeastMaster.Enable", true))
      return;

    // Optionally restrict to hunters if config says so
    if (sConfigMgr->GetOption<bool>("BeastMaster.HunterOnly", true) &&
        player->getClass() != CLASS_HUNTER)
      return;

    ChatHandler ch(player->GetSession());
    std::string msg =
        sConfigMgr->GetOption<std::string>("BeastMaster.LoginMessage", "");
    if (!msg.empty())
      ch.PSendSysMessage("%s", msg.c_str());
    else
      ch.PSendSysMessage("|cff00ff00[Beastmaster]|r Use |cff00ffff.bm|r or "
                         "|cff00ffff.beastmaster|r to summon the Beastmaster "
                         "NPC and manage your pets!");

    // If player is a GM, show extra info
    if (player->GetSession()->GetSecurity() >= SEC_GAMEMASTER)
    {
      ch.PSendSysMessage(
          "|cffffa500[GM Notice]|r You can also use |cff00ffff.npc add "
          "601026|r to spawn the Beastmaster NPC anywhere, and "
          "|cff00ffff.npc save|r to make it permanent.");
    }
  }
};

void Addmod_npc_beastmasterScripts()
{
  new BeastMaster_CommandScript();
  new BeastmasterLoginNotice_PlayerScript();
  new BeastMaster_CreatureScript();
  new BeastMaster_WorldScript();
  new BeastMaster_PlayerScript();
  LOG_INFO("module", "Beastmaster: Registered commands: .beastmaster, .petname "
                     "rename, .petname cancel");
}
