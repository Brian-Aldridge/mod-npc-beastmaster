# Testing & Compatibility Guidance

This document explains how to verify the BeastMaster module remains compatible with the latest AzerothCore.

## Quick Local Build (Linux)

```bash
# Inside your AzerothCore directory
cd azerothcore
mkdir -p modules
# Clone (or update) module
git clone https://github.com/azerothcore/mod-npc-beastmaster.git modules/mod-npc-beastmaster
mkdir build && cd build
cmake .. -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=Release -DWITH_WARNINGS=ON
cmake --build . -j$(nproc)
```

## Runtime Smoke Checklist

1. Start `worldserver` & `authserver`.
2. Create / login a hunter and a non-hunter.
3. Use `.beastmaster` command: NPC should summon and despawn after 2 minutes.
4. Open gossip and browse normal / rare / exotic (depending on config).
5. Adopt a pet, ensure happiness maxes if `KeepPetHappy=1`.
6. Rename a tracked pet using `.petname rename <NewName>` and verify DB update.
7. Delete a pet from tracked list and confirm menu refresh.
8. Toggle `HunterOnly` / `AllowExotic` / `TrackTamedPets` and restart to verify behavior.

## Config Validation Expectations

-   Misordered Min/Max level values auto-correct with a warning.
-   Conflicting HunterOnly + AllowedClasses logs a warning (HunterOnly wins).
-   `MaxTrackedPets` > 1000 logs a performance warning.

## Adding Future Automated Tests

AzerothCore currently lacks an official unit test framework for gameplay scripts. Options:

-   Add minimal C++ integration tests when core gains support (placeholder).
-   Create a Docker-based scripted run that starts worldserver, executes a batch of `.server info`, `.beastmaster`, and parses output for errors.
-   Use a Lua test harness (if you have Eluna) to script command invocations.

## SQL Migration Checks

Whenever you alter the SQL schema:

-   Incrementally apply new SQL in `data/sql/updates/...`.
-   Provide an idempotent CREATE TABLE with IF NOT EXISTS.
-   Add `ALTER TABLE` blocks for new columns with `IF NOT EXISTS` guard (pattern commentary only; implement as needed).

## Performance Notes

-   Large values of `MaxTrackedPets` will increase SELECT + gossip menu build time; keep practical limits (< 500) for production.
-   Profanity list reload occurs only when file mtime changes; keep list modest (<5k entries) to avoid noticeable reload spikes.

## Manual Deprecation Review

Periodically search for changed APIs:

```bash
grep -R "PlayerBeforeGuardian" -n src || true
```

Adjust hooks if AzerothCore renames or removes them.

## Troubleshooting

| Symptom                  | Likely Cause                                      | Fix                                 |
| ------------------------ | ------------------------------------------------- | ----------------------------------- |
| Beastmaster gossip empty | Config disabled or no rows in `beastmaster_tames` | Enable module / import SQL          |
| Exotic pets missing      | Hunter only and no Beast Mastery talent           | Learn talent or set `AllowExotic=1` |
| Rename fails             | Invalid/profane name                              | Use allowed characters only         |
| Summon cooldown message  | Command re-used too quickly                       | Wait for configured cooldown        |

## Continuous Integration

See `.github/workflows/compatibility.yml` which builds the module daily against latest master.
