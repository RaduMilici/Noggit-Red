# Stage 2: local NPC and quest authoring

For the current Test Here / Test NPC / Test Quest workflow, see
[one-click local testing](../../../etc/creator-test/README.md).

This code targets the bundled Tortoise/VMaNGOS schema, not arbitrary remote or
Wrath databases. Open a Classic/Vanilla project inside the managed Creator bundle.
NPC, quest and item saves go directly to the local database. Original NPCs, quests and
items are never changed: clone or copy them instead. Linking a quest to an original NPC
gives that NPC the quest-giver role.

## Designer workflow

1. Start Noggit from `Creator/Noggit`; wait for LOCAL SERVER to show Running.
2. Enter the creature editor. The bottom panel (NPC Model Picker) lists every NPC with a
   3D preview; **Only my NPCs** filters to yours (shown in bold). Its NPC card on the
   right holds every action, so no right-click is needed (right-drag turns the camera).
3. **Humanoid** / **Creature** design a new NPC with a 3D preview; **Clone** starts from
   the selected NPC. Saving it starts placement: click where it should stand (Esc
   cancels). **Place** (or double-clicking the preview) places it again.
4. Click an NPC in the world to select it. Drag to move; while dragging, scroll to
   rotate (Shift: larger steps, Ctrl: 45-degree steps). The card's placement buttons
   duplicate (click where the copy goes), remove, or locate that placement.
5. **Quests** opens the quest browser on the NPC's quests, **Chain** its quest chain
   diagram. See [Quests](#quests).
6. **Test in game → At this NPC** or **At a spot** (click anywhere in the world) saves,
   restarts the local runtime and launches WoW there. **Test Local** in LOCAL SERVER
   launches without moving your character.
7. Select the WoW executable once. Its relative path is remembered. The launcher
   backs up `realmlist.wtf` once and points the client at the local realm.

8. When it works in game: **Local changes** (status bar) → **Sync to Production**. See
   [Sync to Production](#sync-to-production).

Log in with a local account: **Client Profiles… → Create Local Account…**, or accept the
prompt Test Local shows when no account exists. Accounts are created with the server's
own hash (`AccountService`, `AccountDialog.*`) and only exist in the local database.

## Implementation / files

- `Services.hpp/.cpp`: CreatureService, SpawnService, EquipmentService, AccountService;
  typed drafts, validation, local allocation, safe field copying, and persistence.
- `ContentStore.hpp/.cpp`: the quest and item editors' reads and writes: runs the pure
  plans below under Creator ownership, the recovery journal and Local Changes.
- `../quest/`, `../item/`, `../content/SqlText.*`: pure SQL planning for quests (template,
  givers/enders, drops, scripted events, chains, save plan) and items. Ported from the
  ssh-tunnel branch; unit-tested in `../../../test/quest`.
- `../ui/quest/`, `../ui/item/`, `../ui/content/`: quest browser, six-page quest editor,
  chain diagram, item editor, shared pickers and style. `ContentSession` opens the lists.
- `AppearanceService.cpp`: named factions and valid looks from loaded client DBCs.
- `Database.hpp/.cpp`: dedicated loopback connection, advisory allocation lock,
  Creator ownership metadata, durable before-image recovery journal.
- `ChangeTracker.hpp/.cpp`: Local Changes list, persistence, net-change merging, SQL generation.
- `ChangeExport.hpp/.cpp`: change package export (`manifest.json`, `changes.sql`).
- `LocalChangesPanel.hpp/.cpp`: status-bar "Local changes" button and panel.
- `SqlConnection.hpp/.cpp`: the MySQL client connection shared by the local database and the sync.
- `Ssh.hpp/.cpp`: system OpenSSH client: hardened arguments, tunnel, remote command, host keys.
- `ProductionProfile.hpp/.cpp`: production server settings and the database password store.
- `ProductionSync.hpp/.cpp`: Sync to Production (backup, apply, rollback, restart) and Test Connection.
- `ProductionDialogs.hpp/.cpp`: production settings, host-key check, sync confirmation and progress.
- `AuthoringDialogs.hpp/.cpp`: named selectors, humanoid/creature/clone flows,
  body preview, three weapon slots, outfits, combat settings.
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

NPC writes: `creature_template`, `creature`, `creature_equip_template`, and
`creator_content` (new). Optional clone copies also insert into `npc_vendor` and `npc_trainer`.
Quest and item writes: `quest_template`, `creature_/gameobject_questrelation` and
`_involvedrelation`, `areatrigger_involvedrelation`, `quest_start_scripts` /
`quest_end_scripts` with their lines in `broadcast_text` (IDs from 6,000,000, above the
game's own), quest-only rows in `creature_/gameobject_loot_template` (a source without
loot gets a loot table keyed by its entry), `item_template` (new items, `start_quest`),
and the quest-giver bit of `creature_template.npc_flags`. Gossip and AI scripts are
untouched. Custom outfits live in `Workspace/creator-outfits.json`.

Ownership is the `creator_content` table (kinds `npc`, `spawn`, `quest`, `item`), not ID
ranges: Turtle's own content already uses IDs above 1,000,000.

Identifiers allocate above the existing maximum and at least 1,000,000, under a
MariaDB advisory lock. NPC IDs stay within signed mediumint range for kill targets;
quest IDs within the signed quest-chain columns (8,388,607).
All Creator writers use this lock; external SQL writers are outside this contract.
The native world tables are MyISAM, so rollback uses persisted before-images,
not misleading SQL transactions. `Workspace/creator-recovery.json` restores an
interrupted save on the next Creator database operation. Keep this file with its
matching database. The worldserver reads new content after restart. This journal
is not a general migration or backup system.

## Local changes and export

The status bar shows **Local changes: N**; click it to see what was changed locally:

```
+ NPC: Restless Miller          (created)
~ NPC: Farmer John              (edited)
+ Quest: Beneath the Mill
~ Spawn: Restless Miller moved
- Spawn: Old Farmer placement removed
```

Tracked: NPC create/update/delete, placement create/move/update/delete, quest
create/update/delete (including chain links a save changes on other quests), and item
create/update/delete, saved through Noggit. NPCs, quests and items can be deleted from
their editors (**Delete NPC…** removes all its placements; an NPC used by a Creator quest,
or an item a quest uses, must be freed first). Original content is never deletable.

Each entry holds the entity type and ID, action (CREATE, UPDATE, DELETE, MOVE), the
before-state (rows at the first tracked change) and after-state (rows now), and a UTC
timestamp. Entries are net changes: edits after creation stay CREATE, creating then
deleting removes the entry, reverting to the original removes it, and a placement edit
that only changes position/orientation is a MOVE. Rows per entity:

- NPC: `creature_template`, its `creature_equip_template`, `npc_vendor`, `npc_trainer`,
  and quest relations to original quests (relations to Creator quests belong to the quest).
- Placement: `creature`.
- Quest: `quest_template`, NPC/object givers and enders, the exploration area trigger,
  accept/hand-in scripts with the editor's spoken lines, quest-only drops of the items it
  collects (plus loot tables the editor gave their sources), and the item that starts it.
- Item: `item_template`.

The list lives in `Workspace/creator-changes.json`, never in the world database. It
is written in two phases around the recovery journal (`creator-changes.pending.json`
is promoted only once the database save has committed), so after a crash the list
matches the database. An unreadable list is set aside as `creator-changes.json.invalid-*`
and reported in the panel. **Clear Selected** asks for confirmation and only removes
list entries; the content stays in the local world.

**Export Changes** asks for a package name, author and folder, and creates e.g.
`Haunted-Mill/` with:

- `manifest.json`: package name, author, creation date (UTC), generator, source runtime
  (server, MariaDB version, platform) and content version (latest Tortoise world
  `migrations` row), every affected entity, and the SHA-256 of `changes.sql`.
- `changes.sql`: only the exported changes, never a dump. Upserts use `REPLACE INTO`
  with full rows; deletions are guarded so they only remove rows that the target
  database also marks as Creator content. Statements are ordered by dependency and
  replaying a package is idempotent. What an edit removed (spoken lines, drops, the
  start item) is removed on import; drops another quest still collects are kept.
  Creator NPCs, items and quests the changes refer to (givers, kill targets, summoned
  creatures, collected/rewarded/starting items, chain neighbours) are included
  automatically and flagged `includedAsDependency`; original content is assumed present.

Export needs the local database running (to find those dependencies and the content version).
An existing package folder is never overwritten. After exporting, the exported entries
can be cleared (with confirmation).

Limits: IDs are kept as allocated locally (1,000,000+), so a target that already holds
different Creator content with the same IDs would be overwritten; there is no ID
remapping, import UI, remote collaboration, Git, or terrain/client file export yet.
Values are exported as the server formats them, so float columns (positions) keep
MariaDB's display precision. Changes made outside Noggit, or before this tracker
existed, are not listed. Native world tables are MyISAM, so applying a package is
not transactional.

## Sync to Production

Design locally → Test locally → **Sync to Production**. Production only changes when you
click Sync; normal editing never touches it.

**Setup (once):** Local changes → **Server…**. Enter the SSH host, port, user and private
key, the world database as the server sees it (host, port, name — `tw_world` for
tortoise-deploy — and user), how the database password is kept, and optionally the
command that restarts the world server. **Test Connection** logs in over SSH and to the
database and only reads: it shows the database version, the world's size, what was synced
before, and warns when production's content version differs from yours, when the
database user has broad rights, or lacks ones a sync needs. The first time, it shows the
server's host-key fingerprints: compare them with your administrator's (on the server:
`ssh-keygen -lf /etc/ssh/ssh_host_ed25519_key.pub`) before trusting them.

Settings live in `Workspace/runtime.ini` (`[production]`), never the password. The
password is asked for when syncing and remembered until Noggit closes, or saved in the
system keyring (Linux: `secret-tool`, from `libsecret-tools`). Trusted host keys go to
`Workspace/ssh/known_hosts`; keys already in your `~/.ssh/known_hosts` are honoured too.

**Sync:** lists the pending changes (and the Creator NPCs, items and quests they rely on,
which are sent too) and the target, and asks to confirm. Then:

1. Connect: `ssh` (batch mode, strict host-key checking, `-F none`) forwards a random
   loopback port to the database on the server; nothing else is opened. A server-side
   lock stops two syncs from overlapping.
2. Backup: every production row the sync can change is read and saved (see below).
   IDs production already uses for content that did not come from Noggit stop the sync
   here, with nothing changed.
3. Apply: the package's statements run one by one in a transaction.
4. Restart: the restart command runs over SSH, if set. Without one, restart the world
   server yourself; it loads NPCs and quests at startup.
5. Only then are the synced entries removed from Local Changes. An entry edited again
   meanwhile stays listed.

If any step fails, nothing is marked as synced, the error and the server's output are
shown, and production is put back: the transaction is rolled back and the backup is
restored (on a fresh connection if the old one died). Restore failure is reported loudly
with the way to finish it by hand. The Tortoise world tables are MyISAM, which ignores
transactions, so the backup is the real rollback. It covers `ChangeTracker::footprint`:
the rows `changes.sql` can change, selected by key columns the script never updates, so
deleting what matches and putting the saved rows back restores them exactly (verified by
table checksums in the integration test). A failed restart also restores the database.

Each sync keeps a folder `Workspace/sync/<date-time>/`: `manifest.json` (with the
destination), `changes.sql`, `backup.json`, `restore.sql` (puts the backed-up rows back),
`sync.log` and `result.json` (`synced`, `failed`, `rolled-back` or `rollback-failed`).

### Server setup (administrator)

Give each designer their own SSH key and a database user limited to the world database.
Nothing needs root, and the database port stays closed to the internet.

1. Make the database reachable from the server itself only. With tortoise-deploy, add to the
   `database` service in `compose.yaml`: `ports: ["127.0.0.1:3306:3306"]`.
2. Create the database user (in the database container, as root):

   ```sql
   CREATE USER 'noggit_sync'@'%' IDENTIFIED BY 'a long random password';
   GRANT SELECT, INSERT, UPDATE, DELETE, CREATE ON tw_world.* TO 'noggit_sync'@'%';
   ```

   `CREATE` is only needed for the first sync, which creates the `creator_content` table.
3. Add the designer's public key to a dedicated, unprivileged account's
   `~/.ssh/authorized_keys`, restricted to the database forward and, optionally, a
   restart script as its only command:

   ```
   restrict,port-forwarding,permitopen="127.0.0.1:3306",command="/home/noggit-sync/restart-world" ssh-ed25519 AAAA... designer
   ```

   The tunnel (`ssh -N`) never runs the command. Sync's restart step runs it whatever
   command is configured in Noggit. Without `command=`, use `command="/usr/sbin/nologin"`
   and leave the restart command empty. A restart script for tortoise-deploy needs Docker
   access, e.g. a `sudo` rule for exactly `docker compose restart mangosd` in that folder.

Out of scope: several designers syncing to one server, merge conflicts, ID remapping,
staging servers, terrain/client files and DBC deployment. tortoise-deploy re-creates the
world database when an upstream migration is edited, which drops synced content. Keep
the `Workspace/sync` folders: their `changes.sql` files can be applied again, oldest first.

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

The quest editor and browser come from the ssh-tunnel branch, writing to the local
database. In the creature editor's NPC card, **Quests** opens it for that NPC. The
browser lists every quest (yours in **bold**), filtered to the NPC's quests when opened
from one. **New**, **Copy**, **Edit**, **Delete** and **Test** do what they say;
double-click edits your quests and copies game quests. **Your items...** manages items.

The editor has a sidebar of pages; a red dot marks a page with something to fix, and
the problem is shown at the bottom. Before anything is written, a confirmation lists
every consequence (e.g. "Young Wolf drops Wolf Pelt (40 %) for players on the quest").

1. **Story**: title, quest level and the level it becomes available at, zone, kind
   (normal, elite/group with a suggested group size, dungeon, raid, PvP), time limit,
   repeatable/shareable, and the texts (`$N` name, `$C` class, `$R` race, `$B` new line).
2. **Who can take it**: Alliance / Horde / everyone, classes, a profession with a minimum
   skill, a reputation range, earlier quests (all or any), and either/or partners.
3. **Objectives**: kill creatures (optionally only when a spell is cast on them), use an
   object (lever, altar), collect items and where they drop (a creature or a chest, with a
   chance; **New item...** makes one), explore a place (pick one or **Nearest to my
   cursor**; places used by another quest, teleports or inns are greyed out), or reach a
   reputation. No objectives: players just talk to the ender.
4. **Rewards**: experience (**Use typical** fills in what the game's quests of that level
   give), money, items given, items to choose from, reputation with up to five factions,
   a spell taught and a spell cast on the player.
5. **Givers**: NPCs or objects (a wanted poster) that give and take the quest (NPCs become
   quest givers), an item that starts it (your own items only), an item handed out when
   accepting, and the quest offered right after this one.
6. **Events**: a timeline for accepting and handing in: the NPC says/yells something,
   emotes, casts a spell, gives an item, summons a creature (at your cursor, for a number
   of seconds), attacks, or completes the quest ("listen to the story" quests).

**Chain...** shows a quest chain as a diagram: drag from a quest's right-edge handle onto
another quest to link them (all / any / instead when it already needs one), right-click
for either/or groups, editing, or a follow-up quest. Changes are saved together.

**Save and test** in the editor (or **Test** in the browser) saves, restarts the local
server and launches WoW next to the quest's giver (see local testing). Kill counts cap at
63 for Vanilla's six-bit progress counter. Escorts and other complex scripts are not
supported; a quest whose stored script has steps the editor cannot show says so, and
saving its events replaces them.

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

Quest and item planning unit tests (no database; the ssh-tunnel branch's suites):

```bash
cmake -S test/quest -B build-quest-tests
cmake --build build-quest-tests -j2
ctest --test-dir build-quest-tests --output-on-failure
```

Local change tracking unit test (no database; covers merging, restart persistence,
crash recovery around the journal, clearing, and SQL escaping/order, quest events,
drops and items):

```bash
cmake -S test/creator -B build-creator-tests
cmake --build build-creator-tests --target change_tracker_tests ssh_tests
ctest --test-dir build-creator-tests --output-on-failure
```

`ssh_tests` covers SSH target validation, argument hardening, error classification,
host-key fingerprints and forgetting, and that names cannot break the generated SQL.

Real database integration checks require a disposable, stopped copy of the bundle.
The test leaves test content in that copy, starts all three services, checks ID
uniqueness, source preservation, names, placement movement/deletion, respawn edits,
quests (events, a quest drop giving a creature its own loot table, a refused self-link,
a follow-up chain), items (create, edit, delete protection), MyISAM rollback, Local
Changes entries, package export (and replaying its `changes.sql` on the live schema),
the sync backup restoring every row a package changed (table checksums) and ID conflicts,
NPC/quest/item deletion and local accounts, then stops.
Do not copy a running MariaDB data directory. Do not run beside another local server.
The copy needs about 7 GB; `/tmp` is often too small, so pick a roomier parent folder.

```bash
cmake -S test/creator -B build-creator-tests
cmake --build build-creator-tests -j2
creator_test_root="$(mktemp -d "${CREATOR_TEST_PARENT:-$HOME}/noggit-creator-test.XXXXXX")"
cp -a build/Creator/Runtime build/Creator/Database build/Creator/Workspace "$creator_test_root/"
touch "$creator_test_root/CREATOR_TEST_DISPOSABLE"
./build-creator-tests/creator_integration "$creator_test_root"
```

Manual acceptance: create a humanoid and a creature, clone an original with the safe
copy defaults and appearance-only, save/reapply an outfit, move/rotate/duplicate/delete,
reopen properties, and make quests with each objective kind, events and a chain. Test locally, reconnect the client, and
verify names/looks/placement, quest acceptance/progress/completion and rewards. Repeat
Test Locally with the server stopped and ensure shutdown leaves no child services.
