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

8. **Gives players → Loot / Vendor / Trainer** on the NPC card (and **Edit Loot…** on a
   chest's menu) edit what it drops, sells and teaches. See
   [Loot, vendors and trainers](#loot-vendors-and-trainers).
9. When it works in game: **Local changes** (status bar) → **Sync to Production**. See
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
- `ItemService`, `LootService`, `VendorService`, `TrainerService` (`.hpp/.cpp`): items as the
  editors show them, and the loot, shop and trainer data of NPCs and objects (load, validate,
  save, drop simulation). No widgets; their checks are unit-tested in `test/creator/ServiceTests.cpp`.
- `ItemBrowser.hpp/.cpp`: the reusable Item Browser, item tooltip and details card.
- `TalentService.hpp/.cpp`: talent trees without widgets: Talent.dbc reading/writing, the game's layout
  and arrow rules, checks, earliest levels, the build preview, comparison, the workspace store and the
  local server's Talent.dbc. `TalentEditor.cpp`: the visual Talent Editor. Tested in `test/creator/TalentTests.cpp`.
- `LootEditor.cpp`, `VendorEditor.cpp`, `TrainerEditor.cpp` (`ServiceEditors.hpp`), `EditorWidgets.*`:
  the visual editors and their shared pieces (money editor, drop target table, row problems).
- `CreatorPreviews.hpp/.cpp`: NPC, object and item 3D previews, on the creature picker's orbit
  viewer (`ui/tools/PreviewRenderer/CreaturePreviewViewer.hpp`, moved out of MapView).
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
and the quest-giver bit of `creature_template.npc_flags`. Dialogue writes are listed under
[Dialogue](#dialogue-gossip). AI scripts are untouched. Custom outfits live in
`Workspace/creator-outfits.json`.

Ownership is the `creator_content` table (kinds `npc`, `spawn`, `quest`, `item`, and for
dialogues `gossip_menu`, `npc_text`, `gossip_text`, `gossip_script`, `condition`), not ID
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
- Dialogue (per NPC): its greeting and gossip flag, and the menus, options, texts, scripts and
  conditions made in Noggit that the greeting leads to.

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

## Loot, vendors and trainers

Open them from the NPC card's **Gives players** row (Loot, Vendor, Trainer) or a chest's
**Edit Loot…** menu entry. Original NPCs and objects open read-only, with **Copy to My NPC…**
to give one of yours the same setup. Nothing needs table names or IDs: items are picked in the
Item Browser or dragged from it, spells by name. Problems show in the row's Notes column (✗
errors block Save, ⚠ warnings do not); problems of the whole list show under it.

**Loot** (NPCs; chests and fishing holes): the NPC or object in 3D, a table of items with
chance, **Always** (guaranteed), quantity range, group and the resulting chance per kill, and
the money it drops. Groups follow the server: exactly one item of a group drops, explicit
chances are taken in order up to 100%, and 0% items share what is left. Quest drops are shown
read-only (the quest editor manages them); shared reference tables are kept and shown as one
row. **Copy Loot…** takes another NPC's or chest's loot (replace or add), **Use on Another…**
gives one of your own the same loot, **Clear** and **Reset** discard. An NPC still using the
loot of what it was cloned from gets its own table on save; the original is never changed.
**Simulate Drops** rolls 10,000 kills (adjustable) with the server's group and reference rules
and lists expected against observed chances, average quantity, average money and empty kills.
Checked: missing items, chances outside 0–100%, 0% outside a group, min above max, duplicates,
groups over 100% or starving their 0% items, money min above max.

**Vendor**: a preview of the game's shop window (two columns of five per page, icons with
stack size and stock, names in quality colours, prices in coins) next to the editable list:
stock (or Unlimited), restock time, price (from the item), order (arrows or **Sort by** name,
price, type, level, quality). **Sells items** gives the NPC the vendor role. A shared original
vendor list is shown and can be made editable (copied into the NPC's own list). **Copy
Inventory…** / **Use on Another…** as for loot. Checked: missing items, duplicates (also against
the shared list), limited stock without restock, more than 128 goods, free items, no vendor role.

**Trainer**: a preview of the trainer window, as one of your local characters or any
character of a level, with the game's **Available / Too High Level / Already Known** filters
and the selected spell's icon, rank, required level, cost, prerequisite and description. The
editable list sets required level and cost (original trainers' values are suggested); **Add
Spell** searches learnable spells by name, **Sort by Level**, **Copy Trainer Setup…**, **Use on
Another…**. **Who can train**: everyone, or one class. Trainers list the spell that teaches a
spell; the editor shows and stores that for you. Checked: spells trainers cannot teach,
duplicates, levels outside 0–60, required level below the spell's own, a previous rank not
taught here, a class trainer without a class, no trainer role.

**Item Browser** (also used for NPC weapons and, through the "…" buttons, quest items):
search by name, icon grid or list, quality, class, type, slot and required-level filters, and
**All / My items / Recent** (recent picks are kept in `runtime.ini`). The selected item shows its
icon, name, quality, the game's tooltip (damage, speed, stats, requirements, item level, sell
price) and its own 3D model when it has one (weapons, shields, held items, helms, shoulders).

**Testing**: **Test Loot / Test Chest / Test Vendor / Test Trainer** place your local test
character beside the NPC's or object's saved placement (choosing one when there are several)
after restarting the local server, which also resets it. Options, applied to the local
character and account databases only, after the restart and before you log in: let the local
account use `.respawn` to reset the NPC without restarting (loot), add test money (vendor,
trainer), set the character's level (trainer).

**Local Changes and sync**: each save is one entry per NPC or object — `+ Loot: Restless
Miller`, `~ Vendor: Brother Alric`, `+ Trainer spell: Frostbolt Rank 2 (Brother Alric)` — that
carries the owner's columns the editor sets (loot table and money, vendor and trainer
settings, only the vendor or trainer role flag) and its rows. Their SQL only runs where the
owner is Creator content (`@owned`), only writes a loot table the owner owns, and removing an
NPC removes its loot, shop and trainer rows first. Older Local Changes entries that still carry
vendor and trainer rows inside the NPC entry keep working.

## Dialogue (gossip)

NPC card → **Dialogue → Edit Dialogue**, or right-click a selected NPC in the world → **Edit
Dialogue…**. A conversation is a tree: a node is what the NPC says, its children are the
player's responses, and a node appears under the first response that leads to it (others say
"back to Node 2"). Select a node or response to edit it on the right; no tables or IDs appear.

```
Greeting · "Something has disturbed the dead."
├─ "What happened?"  →  Node 2
│    └─ Node 2 · "The graves were opened from below."
│         ├─ "I found the bones."  →  End conversation · complete The Missing Graves · if 2 conditions
│         └─ "Something else..."  →  back to Greeting
├─ "Show me your goods."  →  Open vendor
└─ Lists quests: The Missing Graves
```

The left side shows the NPC in 3D and the game's gossip window: click responses to play the
conversation (with Back / Start over); conditional responses say who sees them, effects say what
happens. ✗ problems block Save, ⚠ warnings do not.

What a response can do, all stored the way the Tortoise/VMaNGOS world database already does:

| In the editor | Stored as |
| --- | --- |
| Continue the conversation / End the conversation | `gossip_menu_option` type 1, `action_menu_id` = next menu / -1 |
| Open the shop / Open training | option type 3 / 5 (shown only when the NPC has the vendor / trainer role) |
| List this NPC's quests (greeting) | the quest-giver marker option; the game lists available quests by title |
| Cast a spell on the player | `gossip_scripts` command 15 (the NPC casts it) |
| Teleport the player (map, position, "Use my cursor position") | `gossip_scripts` command 6, targets swapped so the player moves |
| Complete a quest (talk/event quests) | `gossip_scripts` command 7; your own quests get the event flag on save |
| Only shown when… (up to 4, all must hold) | `conditions` rows, combined with AND |

Conditions: player is / is not on a quest, finished its objectives, has / has not completed it,
carries / does not carry an item (count), level at least / at most. Text supports `$N`, `$C`,
`$R` and `$B`. Effects only run on responses that continue or end the conversation (the game
opens shop and trainer windows directly). Quests can only be listed in the greeting: offering a
quest from a later node, escorts/following, custom logic and cinematics are shown greyed out as
**needs scripting support**. Not supported: greetings that change with conditions, random
greetings, confirmation pop-ups and payments, map markers, other services (flight master,
innkeeper, banker…), unlearning talents.

**Copy from NPC…** copies any NPC's dialogue into the editor; **Copy to My NPC…** (on game NPCs,
which open read-only) gives one of yours a copy. Parts the editor cannot show are listed and left
out of copies. A Creator NPC cloned with "dialogue" gets its own copy; game NPCs' dialogues are
shared until edited, and saving then gives the NPC its own copy. Deleting an NPC deletes its
dialogue.

Checked: empty texts, responses without a target, nodes nothing leads to, more than 64 nodes or
15 responses, missing quests/items/spells, a teleport without a destination, completing a game
quest that is not an event quest, impossible level ranges; warned: shop/training responses on an
NPC without that role, a vendor or trainer whose dialogue gives no way to shop or train, quests
the greeting does not list, quest credit without an "is on quest" condition.

**Test Dialogue…** saves, restarts the local server and puts your test character beside the NPC
(asking which placement when there are several). It lists which responses only some players see
and can set your character's level for level conditions.

**Storage, Local Changes and sync**: each node is a `gossip_menu` with an `npc_text` and its line in
`broadcast_text`; responses are `gossip_menu_option` rows, effects `gossip_scripts`. Saving
replaces the NPC's previous version, reusing its IDs (a menu ID must fit the signed
`action_menu_id`). Conditions are unique by value in the world database, so an existing identical
condition is reused and new ones are only added, never deleted. One Local Changes entry per NPC —
`+ Dialogue: Brother Malric · 3 nodes` — carries the greeting, the gossip flag and the rows
marked as made in Noggit. Its SQL runs only where the NPC is Creator content, only removes rows
the target also marks as made in Noggit (a game NPC's dialogue is never removed), and adds
conditions with `INSERT IGNORE`. Before a sync, an ID production already uses for other content,
or a condition production stores under another ID, stops the sync with nothing changed. The
backup covers every dialogue row and mark the sync can change. A Creator quest the dialogue
completes is listed and synced with its event flag; the quest editor keeps that flag while a
dialogue completes the quest.

## Items and spells

NPC card → **Library → Items / Spells**, right-click the world → **Create → Item… / Spell…**, or
the **New / Clone / Open** buttons of the Item Browser and the spell picker, which every editor uses
(Loot, Vendor, quest rewards, NPC weapons, item spells, triggered spells). Whatever is saved there
can be picked at once. Game items and spells open read-only, with **Clone This Item / Spell**.

**Item Editor.** New items start from **Clone Existing Item** (any game item: look, stats and
effects) or a template — Weapon, Armor, Consumable, Quest Item, Crafting Material, Key,
Trinket. The left side shows the 3D model and the game's tooltip as you type. Pages:
*Basics* (name, description, quality, item and required level, kind and type, where it is
worn — only slots that fit the kind —, binding, stacking and carry limit, vendor prices,
**Use the Look of Another Item…** for the icon and model), *Combat* (armor, block, weapon
damage, school and speed with damage per second, durability), *Stats* (up to ten, and six
resistances), *Who can use it* (classes, races), *Spells* (when used — uses, used up,
cooldown —, while worn, chance on hit with procs per minute). Every `item_template` column the
editor does not show is kept as cloned. Checked: worn slot against the kind, weapon speed and
damage, stacking worn items, missing spells, nobody able to use it; warned: sell above buy,
stats on unworn items, missing durability or look. **Test Item…** puts one or more in your test
character's backpack (and can set its level) and starts WoW where the character logged out.
**Delete Item…** works for your items nothing uses.

**Spell Editor.** **Clone Existing Spell**, or **Create Spell** from an effect template: Direct
Damage, Heal, Damage/Healing over Time, Buff, Debuff, Stun, Root, Silence, Fear, Summon, Trigger
Spell, Learn Spell, Energize — each set up the way most of the game's own spells of that kind
are (target, range, duration, cast effects, icon). Pages: *Basics* (name, rank, school,
description — **Write it for me** fills in `$s1` / `$o1` / `$d` —, buff tooltip, **Use the Cast
Effects of…** another spell), *Casting* (cast time, cooldown and global cooldown, range, cost and
power, target type for all effects, duration, stacks, crowd-control kind, proc chance, level),
*Effects* (the three Vanilla effect slots: what it does, aura, who it affects, radius, amount
range, tick time, the spell it casts or teaches, and the creature, stat, power, schools or item
it needs), *Ranks & trainers*. Names are readable throughout; anything a cloned spell uses that
the editor does not offer is kept and shown as "kept as copied". Dummy and script effects are
marked **needs scripting support**. Cast times, durations, ranges and radii are the client's own
rows (`SpellCastTimes`, `SpellDuration`, `SpellRange`, `SpellRadius`): new values would need
server and client table changes, so they are chosen from what exists. The left side shows the
in-game tooltip with the values filled in, and the spell's rank chain.

**Ranks.** **Create Next Rank…** copies the spell with a new level, damage/healing (a percentage),
cost and cooldown, and requires the current rank; it works on game spells too (Fireball's next
rank joins Fireball's chain). Ranks are kept in `spell_chain` as the server expects (a first rank
of yours gets its row). **Trainers can teach it** makes the teaching spell trainers list (a copy
of how the game's trainers teach), so the Trainer Editor offers the spell. **Test Spell…** teaches
the spell and its earlier ranks to your test character (General tab of the spellbook).

Storage: `item_template`; `spell_template` and `spell_chain` (IDs from 1,000,000, below the
signed-mediumint limit `spell_chain` has). Local Changes lists `+ Item: …`, `+ Spell: … (Rank 2)`
and `+ Spell (trainer lesson): …`; export and sync include the Creator spells and items the
changes use (an item's spells, a quest's reward spell, a trainer's lessons, a spell's next rank).
Not supported: custom models or icons, new cast-time/range rows, custom classes,
advanced spell scripting. Talents: see [Talent trees](#talent-trees).

## Client data

Items need no client data (the client asks the server). Spells do: the client only shows and
casts spells in its `Spell.dbc`. Edited talent trees need `Talent.dbc` in both the client (in the same
test patch) and the local server (see [Talent trees](#talent-trees)). Creator writes your spells' rows from `spell_template` (the
two share their columns, checked against all 27,917 bundled spells) into a test patch in the
local client's `Data` folder: `patch-Z.mpq`, the last patch the 1.12 client loads, so it wins.

- A `patch-Z.mpq` the client already has is first backed up to `Workspace/client-data/original/`
  (byte for byte, checksum recorded); Creator's patch contains all its files, and its
  `Spell.dbc` is built on the client's (its rows, e.g. a hand-made spell, are kept). Replacing
  your own patch later is noticed: it is backed up too, the older backup kept.
- **Test Locally** (and Test Spell) updates the test patch when spells changed. **Play
  Production** puts the client's own `patch-Z.mpq` back first (or removes Creator's).
- **Local changes → Client data** lists what the test client does not have yet (`+ Spell.dbc:
  Holy Smite (Rank 2)`), with **Update Test Client**, **Restore Original** and **Export Client
  Patch…** (the patch to hand to players). State is kept separately for each client in `Workspace/client-data/clients/` (existing
  `state.json` files are read for migration),
  apart from Local Changes.
- **Sync to Production does not send client data**; its confirmation warns when synced spells need
  the patch. On Windows, replacing `patch-Z.mpq` needs the client closed (and fails while another
  program has it open).

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
cmake --build build-creator-tests --target change_tracker_tests ssh_tests service_tests talent_tests
ctest --test-dir build-creator-tests --output-on-failure
```

`talent_tests` covers Talent.dbc reading and writing, the game's arrow routing, every layout check,
earliest levels, the build preview, comparison and templates; given the server's dbc folder (ctest passes
`build/Creator/Runtime/mangosd/data/dbc`) it also checks that all 27 game trees pass the checks and that
installing and restoring the server's Talent.dbc is byte-exact (on a temporary copy).

`service_tests` covers loot validation, group chances, the drop simulator, vendor and trainer
checks, dialogues (rows written and read back, validation, conditions, effects), client tables
(WDBC reading and writing, the Spell.dbc layout, patched tables keeping the client's rows,
readable cast times), spell values, templates and checks, item templates and checks, and their
Local Changes summaries, sync SQL and backup footprint. It links the system StormLib
(`libstorm-dev`). `ssh_tests` covers SSH target validation, argument hardening, error classification,
host-key fingerprints and forgetting, and that names cannot break the generated SQL.

Real database integration checks require a disposable, stopped copy of the bundle.
The test leaves test content in that copy, starts all three services, checks ID
uniqueness, source preservation, names, placement movement/deletion, respawn edits,
quests (events, a quest drop giving a creature its own loot table, a refused self-link,
a follow-up chain), items (create, edit, delete protection), MyISAM rollback, Local
Changes entries, package export (and replaying its `changes.sql` on the live schema),
loot, vendor and trainer round-trips, the sync backup restoring every row a package changed
(table checksums) and ID conflicts,
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


## GameObjects and Patrol Studio

Right-click the world → **Create → GameObject**. Browse existing appearances with a
3D preview, then set the name, type, size, state, respawn, faction and optional quest
relation. Click the world to place it. IDs, GUIDs and server coordinates are allocated
by the local service. The GameObject browser retains its visual move/rotate controls;
the context menu adds edit, duplicate, delete placement, locate, save and **Test
GameObject**. Definition edits affect all placements of that definition. Save pending
placements before editing definitions. Loot editing and custom models are not included.
Trap/resource presets inherit the existing behavior of a matching selected template;
Custom uses the selected template's existing behavior.

For **Interact with Ancient Chest**, create a **Quest Object** using a chest appearance.
In Quests → Objectives → **Interact with a GameObject**, search for its name. Tortoise
uses the interactive (goober) type for interaction credit; ordinary loot chests remain
available as chest objects. No numeric objective IDs are needed.

Select any saved NPC placement → **Edit Patrol** (NPC Studio or world context menu).
The panel starts new paths at the NPC's spawn position. Choose **Walk** or **Run** for
the entire path, then click terrain to add nodes. Drag the numbered nodes to reposition
them. Select a node to insert after it, delete it, move it earlier/later, set its departure
speed, wait or facing. **Loop path** closes the route; disabling it stops at the last
node. Closing with unsaved edits offers Save / Discard / Cancel.

**Save patrol locally** journals the placement, waypoint rows and private movement
scripts together. This also works on original NPCs without cloning their template;
only that placement gets the new path. Existing template paths are read as a starting
point; unrelated script actions are copied rather than changing shared scripts. A
zero-duration wait uses a one-tick handoff internally so Tortoise applies the speed
script before starting the next segment. **Save & Test NPC** starts local testing.
Existing scripted AI may independently take control of an NPC's movement in game.

GameObjects, their placements, and patrols appear in **Local Changes** and use the
existing database-only SSH sync and rollback. A first sync of an original NPC's patrol
requires the production placement/path to match the captured local baseline. Script-ID
collisions are reported before applying changes. Client and world files are not sent.

Targeted checks (no server):

```bash
cmake -S test/creator -B build-creator-tests
cmake --build build-creator-tests --target change_tracker_tests -j2
ctest --test-dir build-creator-tests -R '^change_tracker$' --output-on-failure
```

The disposable-runtime `creator_integration` checks also cover GameObject placement and
movement, a named interaction objective, walk/run/wait/loop persistence, repeated saves,
and authoring an original NPC's patrol, spells and items (a teachable spell trainers find, its next
rank, a potion casting it, a cloned weapon, deletion protection), and dialogues: a round trip with a condition, shop,
training and a spell, unchanged saves keeping their rows, read-only game NPCs, a clone getting
its own copy, deletion with the NPC, and the sync backup restoring every dialogue row. Use the
disposable-copy instructions above;
that suite is deliberately not run against the designer's workspace.

After building, manually verify: chest preview and placement; move/rotate/duplicate/delete;
interaction credit in the local client; walking and running on a multi-node patrol;
insertion, dragging and reordering; stopping versus looping; and reload persistence.
Full application build, visual rendering, client behavior and SSH execution were left
for the developer to run.

## Talent trees

NPC card → **Library → Talents**, or right-click the world → **Create → Talent Trees…**. The editor is the
game's talent window with editing unlocked: the class's three trees side by side on their own paintings
(double-click a tree's title, or use its toolbar button, to zoom in), real icons, `0/5` rank badges,
the game's arrows, and the points each row needs down the left side.

**Edit Tree.** Drag a talent to move it (onto another talent: they swap; arrow keys move the selected one).
Drag the handle under a talent — or Shift-drag the talent — onto the talent that should require it.
Click an arrow to change the required talent, the points needed, or remove it. Right-click an empty slot:
**From Existing Spell**, **Clone Existing Talent** (any class; the clone gets its own copies of the rank
spells), **Create New Talent** (a new passive talent spell, set up like the game's). Right-click a talent
for its icon, ranks, requirement, replace, duplicate and delete. Click a talent for its side panel:
name, icon (searchable grid), every rank with **Open** (the Spell Editor), **Clone Previous Rank** (a copy
with values raised one step: 1, 2, 3…), **Replace**, **Create Next Rank**, the requirement, the in-game
tooltip at any rank, and how to reach it. Hovering lights up a talent's whole prerequisite path and shows
the game's tooltip. Everything saves as you go; **Undo/Redo** (Ctrl+Z / Ctrl+Y) cover every tree edit.

**Character Level and Test Build.** The level slider (10–60) sets the points (level 10 gives the first,
each level one more). In **Test Build** clicking spends a point and right-clicking gives one back, with
the game's rules: points left, 5 points per row in that tree, the prerequisite's points, the maximum rank;
points that hold a row or a requirement open cannot be given back. Locked talents are grey, ones that can
take a point green, maxed ones gold. Lowering the level gives back the points learned last. In Edit Tree,
each talent shows its earliest level (`Lv 40`), recomputed on every change; the side panel shows the points,
the tree points and the prerequisite path.

**Compare with Original** marks new, moved (with where it was) and changed talents, removed talents and
arrows, and lists the changes. **Clone Tree as Template** copies a tree into a template (in the class list):
edit and test it like any tree; it never goes into the game until **Replace With Template** puts it into a
class tree. **Reset From Original** puts the game's own tree back (Undo still works).

**Test In Game…** checks the trees, puts them into the local server and test client, optionally sets the
test character's level, resets its talents of this class and gives it the build shown, and starts WoW.

### Implementation boundary

The target is the Tortoise/VMaNGOS 1.12 server with Turtle's 1.18 client, whose `Talent.dbc` and
`TalentTab.dbc` match the server's (checked when this was written). These rules are the client's and
server's, not Creator's choices:

| Fixed by the game | Value | Source |
| --- | --- | --- |
| Rows × columns | 8 × 4 | Turtle's `Blizzard_TalentUI` (`MAX_NUM_TALENT_TIERS`, `NUM_TALENT_COLUMNS`) |
| Talents per tree | 20 | the frame's talent buttons (more: "Too many talents in talent frame!") |
| Ranks | 1–5 | the server reads five rank spells (the table has room for nine) |
| Points per row | 5 | client and server; **"points required" follows the row**, so it changes by moving |
| Prerequisites | one, same tree, above or same row | the server reads the first; the client draws only these |
| Arrows | the client's routing | straight, sideways, across-then-down, else down-then-across |
| Talent points | level − 9 (51 at 60) | |

In scope: moving, adding, cloning, replacing and deleting talents in the existing trees of the nine
classes; ranks and their spells (through the Spell Editor and Creator spells, which reach the client's
`Spell.dbc` as before); icons (of the rank spells); prerequisites; templates; the level simulator, build
preview, reachability and comparison; local testing; exporting the server table.

Not in scope, and why:

- **New trees, renaming trees or their paintings, a fourth tree per class**: `TalentTab.dbc` is read
  only; the server keeps three trees per class. Templates cover experimenting.
- **Moving a talent to another tree**: clone it there (Clone Existing Talent) and delete the original.
- **Other points per row, more rows or ranks, several prerequisites**: hard-coded in the client UI and
  the server; changing them needs client UI and server code changes.
- **Talents that need server scripts**: copies of game spells keep their effects, but scripted ones
  (Deep Wounds' bleed, for example) are tied to the game spell's ID; the editor warns.
- **Changing the game's own spells in place**: as everywhere in Creator, game spells are read-only.
  Changing a talent's icon, name or values first copies its ranks into your spells (**Make Ranks
  Editable**); the game's spells stay as they are.
- **Production**: Sync to Production sends no talent data. **Export Server Table…** saves `Talent.dbc`
  for the server's dbc folder; **Local changes → Export Client Patch…** includes it for players. Existing
  characters keep the talent spells they learned: reset their talents after changing a tree.
- **Live tooltip while editing a spell**: the Spell Editor stays a dialog with its own live tooltip; the
  talent's tooltip updates when it closes.

### Validation and error states

Errors stop **Test In Game**, the server/client install of a local test and **Export Server Table**,
because the game would misbehave; warnings do not. Talents with problems get a red (error) or amber
(warning) frame and a `!` dot; the tree title counts them; the side panel and the tree overview list them
(click one to select the talent).

| Check | Kind | Shown as |
| --- | --- | --- |
| More than 20 talents in a tree | error (creating the 21st is refused) | tree problem |
| Outside the 8 × 4 window, or two talents in one slot | error | talent |
| No ranks, more than 5, or a rank without a spell | error | talent |
| A rank spell missing from the local database | error (checked while the database runs) | talent; "missing" in the rank list |
| A rank spell used twice, or by a talent of another tree | error (the server maps a spell to one talent) | talent |
| Prerequisite not in the tree, below the talent, itself, or in a circle | error (refused when linking) | talent; red line while linking |
| Requires more points than the prerequisite has ranks | error (lowering ranks lowers it) | talent |
| An arrow the game cannot draw (a talent in its way) | error (in game: an error popup whenever the window opens) | dashed red arrow |
| Cannot be learned by level 60, or its row cannot be opened with the talents above | warning | `unreachable` and a red cross |
| A copied rank whose effects need server scripting | warning | talent |
| Build over the level's points | shown | "Over budget" in the level bar |
| Learning or giving back a point the game would refuse | refused | the reason in the status line |

Other states: without the managed runtime the editor is disabled with a notice; if the server's tables
cannot be read, a banner says why; with the local database stopped, layout editing works but names,
icons, descriptions and spell changes wait for it (a banner, and each action that needs it says so). A
failed save shows "Not saved" in red. Dropping outside the window, linking to an invalid talent, or adding
to a full tree leaves the tree unchanged and says why.

### Storage

`Workspace/creator-talents.json` holds the edited trees (whole trees by TalentTab ID; a tree equal to the
original is not stored) and the templates. The originals are the server's
`Runtime/mangosd/data/dbc/Talent.dbc` (once Creator has replaced it: its backup in
`Workspace/talents/original/`, checksum in `Workspace/talents/server.json`). Before a local test's restart
Creator writes `Talent.dbc` there (the server reads it at startup); the client test patch gets the client's
own `Talent.dbc` with the same trees. With no edited trees the server's own file is put back byte for
byte. New talent IDs go above every ID in use. Talent rank spells get no `spell_chain` rows: the server
builds talent rank chains from `Talent.dbc`.
