# Stage 2: local NPC and quest authoring

This code targets the bundled Tortoise/VMaNGOS schema, not arbitrary remote or
Wrath databases. Open a Classic/Vanilla project inside the managed Creator bundle.
NPC and quest saves go directly to the local database. Original NPC templates are
read-only in the properties editor; explicit quest assignment can add quest-giver
flags and relations to an original NPC.

## Designer workflow

1. Start Noggit from `Creator/Noggit`; wait for LOCAL SERVER to show Running.
2. Right-click terrain: **Create → NPC**. Choose clone, humanoid, or creature.
3. Choose an existing look, edit appearance and combat separately, and save.
4. In NPC mode, drag to move. While dragging, scroll to rotate (Shift: larger
   steps, Ctrl: 45-degree steps). Right-click a selected NPC to edit,
   duplicate, delete, locate, or save placements. Duplicates share the NPC's
   properties; clone instead when a separate NPC definition is wanted.
5. Right-click an NPC → **Create / Edit Quest**, or **Create → Quest** on terrain.
6. **Test Locally** saves pending NPC placements and restarts the local runtime.
   Wait for all services to reach Running, then **Launch Local Client**.
7. Select the WoW executable once. Its relative path is remembered. The launcher
   backs up `realmlist.wtf` once and points the client at the local realm.

No teleport, account creation, client distribution, or automatic login is added.
Use the local account provided by the prepared runtime and travel to the NPC.

## Implementation / files

- `Services.hpp/.cpp`: CreatureService, SpawnService, EquipmentService, QuestService;
  typed drafts, validation, local allocation, safe field copying, and persistence.
- `AppearanceService.cpp`: named factions and valid looks from loaded client DBCs.
- `Database.hpp/.cpp`: dedicated loopback connection, advisory allocation lock,
  Creator ownership metadata, durable before-image recovery journal.
- `AuthoringDialogs.hpp/.cpp`: named selectors, humanoid/creature/clone flows,
  body preview, three weapon slots, outfits, combat settings, quest editor.
- `../MapView.h/.cpp`: click placement, world editing and save integration. Reuses
  existing NPC drag/rotate/rendering, coordinate conversion and selection systems.
- `../runtime/LocalClientLauncher.hpp/.cpp`: client selection, realm setup, launch.
- `../runtime/RuntimeManager.hpp/.cpp`, `LocalServerPanel.cpp`: save-before-test hook,
  startup when stopped, restart and launch actions.
- `../../../cmake/CreatorRuntime.cmake`: packaged Creator requires database support.
- `../../../test/creator`: opt-in real-runtime integration executable.
- `../../../test/runtime/CMakeLists.txt`: includes the launcher used by the panel.
- `../../../test/runtime/RuntimeTests.cpp`: cold-start Test Locally and save cancellation coverage.

## Database and metadata

Writes: `creature_template`, `creature`, `creature_equip_template`, `quest_template`,
`creature_questrelation`, `creature_involvedrelation`, and `creator_content` (new).
Optional clone copies also insert into `npc_vendor` and `npc_trainer`.
Reads: those tables plus `item_template`. Loot/gossip definitions are referenced,
not edited; script tables are untouched. Custom outfits live in `Workspace/creator-outfits.json`.

Identifiers allocate above the existing maximum and at least 1,000,000, under a
MariaDB advisory lock. NPC IDs stay within signed mediumint range for kill targets.
All Creator writers use this lock; external SQL writers are outside this contract.
The native world tables are MyISAM, so rollback uses persisted before-images,
not misleading SQL transactions. `Workspace/creator-recovery.json` restores an
interrupted save on the next Creator database operation. Keep this file with its
matching database. The worldserver reads new content after restart. This journal
is not a general migration or backup system.

## Supported appearance and combat

- Search existing NPCs by name, inspect levels/type/faction/look/role/equipment.
- Choose the six safe copy groups; appearance-only copies body and visible weapons.
- Loot, vendor, trainer, gossip and quest associations can be copied explicitly;
  all are unchecked by default. Existing definitions remain shared. AI/script
  copying is disabled because it may depend on the source identity. Sources remain unchanged.
- Humanoid race, sex, skin, face, hairstyle, hair color and facial hair select
  existing valid combinations. A feature change can change other features and
  baked clothing because no new client rows are generated.
- Creature looks use existing model/display combinations; no model import.
- Body textures/geosets preview in 3D. Attached armor and weapons do not preview
  in this dialog; verify them on the placed world model and in the client.
- Main hand, off hand and ranged items are searched by name and slot compatibility.
  Other armor is baked into existing looks; independent armor-slot editing and
  arbitrary player item-set conversion are not supported.
- Apply existing NPC looks as named outfits, or save/reuse custom local presets.
- Class profiles offer editable health/resource/damage defaults, without spell AI.
  Mana above zero selects Mana; at zero, Rogue uses Energy and other classes Rage.
  Visible equipment does not import player combat stats. Guard uses native guard
  behavior; Vendor/Trainer roles have no inventory/lesson editing, but clone options
  can copy existing inventories/lessons.
- Editing an NPC's respawn/movement defaults updates all placements of that NPC.

## Quests

Create/edit Creator quests with title, required/quest level, giver/ender, description,
completion text, XP and gold/silver/copper rewards. Up to four kill/collect objectives
use searchable NPC/item selectors. Kill counts cap at 63 for Vanilla's six-bit
progress counter. Talk quests are single-objective delivery/turn-in quests with
no custom conversation script. Relations and giver flags are saved automatically.
Collect objectives require already-obtainable items; this stage does not add drops.

## Build and verification

Full builds and runtime tests were intentionally left for the developer to run.
Targeted C++ syntax checks passed during implementation; these do not verify linking,
OpenGL display, live database writes, or in-game behavior.

From `/home/radu/Documents/Noggit-Red`, close Noggit before replacing its executable:

```bash
cmake -S . -B build
cmake --build build -j2
cp build/bin/noggit build/Creator/Noggit/noggit
(cd build/Creator/Noggit && ./noggit)
```

Configure again before building so the recursive source collection includes the
new Creator files. The existing development bundle is reused; this does not create
a new redistributable package. Linux Windows-client launch additionally requires
a working portable Wine distribution at `Runtime/Wine/bin/wine`; it is not supplied
by this change. Windows launches the client directly.

Runtime regression suite (stop Noggit/runtime first; tests use its ports):

```bash
cmake -S test/runtime -B build-runtime-tests
cmake --build build-runtime-tests -j2
ctest --test-dir build-runtime-tests --output-on-failure
```

Real database integration checks require a disposable, stopped copy of the bundle.
The test leaves test content in that copy, starts all three services, checks ID
uniqueness, source preservation, names, placement movement/deletion, respawn edits,
kill/collect/talk quest round-trips, invalid inputs and MyISAM rollback, then stops.
Do not copy a running MariaDB data directory. Do not run beside another local server.

```bash
cmake -S test/creator -B build-creator-tests
cmake --build build-creator-tests -j2
creator_test_root="$(mktemp -d /tmp/noggit-creator-test.XXXXXX)"
cp -a build/Creator/. "$creator_test_root/"
touch "$creator_test_root/CREATOR_TEST_DISPOSABLE"
./build-creator-tests/creator_integration "$creator_test_root"
```

Manual acceptance: create a humanoid and a creature, clone an original with the safe
copy defaults and appearance-only, save/reapply an outfit, move/rotate/duplicate/delete,
reopen properties, and save each quest type. Test locally, reconnect the client, and
verify names/looks/placement, quest acceptance/progress/completion and rewards. Repeat
Test Locally with the server stopped and ensure shutdown leaves no child services.
