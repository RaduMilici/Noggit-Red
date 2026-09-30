# Creating NPCs, quests and items

With a MySQL world database connected (**Settings → MySQL**, optionally through the
[SSH tunnel](ssh-tunnel.md)), Noggit can create NPCs, quests and items and write them straight into the
database. No SQL knowledge is needed: every field is a normal form field, and before anything is written you
get a confirmation listing what will happen and which database it goes to.

Supported world databases: vmangos and Turtle WoW (fully), CMaNGOS, AzerothCore and TrinityCore (fields
their tables store elsewhere are greyed out; scripted events are vmangos-style only).

Only things made with the editors can be changed or deleted. The game's own NPCs, quests and items stay
intact -- copy them instead. What counts as yours is decided by ID:

- **vmangos** (and CMaNGOS / AzerothCore / TrinityCore): IDs from **90000** up (texts from 900000 up).
- **Turtle WoW / tortoise-wow**: IDs from **6000000** up, for NPCs, quests, items and texts alike. The Turtle
  data already uses IDs far above 90000 (NPCs up to 2509000, quests up to 1140820), and those are the game's.
  Noggit recognises a Turtle-style world database automatically (its `creature_template` has `display_id1`
  but no `patch` column).

## NPCs

Open the **NPC Model Picker** and select an NPC in the list.

- **New NPC from selected...** makes a new NPC that starts as an exact copy of the selected one, including
  how it fights, what it drops and its abilities. Change what you need:
  - **Who**: name, title (the `<Blacksmith>` line), level range, rank (normal / elite / boss / rare).
  - **Looks**: the model ID with a live 3D preview, and size.
  - **Behaviour**: the faction, picked by name ("Stormwind -- friendly to Alliance, hostile to Horde"), and
    what players can do with it: talk, quests, vendor, repair, trainer, flight master, innkeeper, ...
  - **Dialogue**: what it says when clicked, and the line above its quest list. If other NPCs share the
    greeting (common for copies), changing it gives this NPC its own; the others keep theirs.
  - **Also copy**: the items the original sells and the spells it teaches.

  A copied NPC does **not** keep the original's special server script (boss fights, events) unless you tick
  the option for it.
- **Edit NPC...** changes an NPC you made.
- **Delete NPC...** removes an NPC you made, with its spawns in the world, its dialogue and its quest links.

## Quests

Click **Quests...** in the NPC Model Picker. With an NPC selected, the list shows that NPC's quests and
**New quest** makes it the giver and the one players return to. Your quests are shown in **bold**.
**New**, **Copy**, **Edit** and **Delete** do what they say (double-click edits your quests and copies game
quests).

The quest editor has a sidebar of pages. A red dot marks a page with something still to fix; the problem is
also shown at the bottom.

1. **Story** -- title, quest level and the level it becomes available at, zone, kind (normal, elite/group
   with a suggested group size, dungeon, raid, PvP), a time limit, whether it can be repeated or shared, and
   the texts. In texts, `$N` is the player's name, `$C` their class, `$R` their race, `$B` a new line.
2. **Who can take it** -- Alliance / Horde / everyone, classes, a profession with a minimum skill
   (fishing, cooking, ...), a reputation (at least / at most a rank with a faction), **earlier quests**, and
   **only one of these** (see *Quest chains*).
3. **Objectives** -- add as many as the database allows:
   - **Kill creatures** (optionally counting only when a spell is cast on them: "use the net on 8 birds").
   - **Use an object** -- levers, altars, braziers ("Light the brazier").
   - **Collect items** -- and say where they come from: **dropped by** a creature or **found in** a chest,
     with a chance. The item then drops only for players on the quest, like the game's quest items. Every
     creature (or chest) of that kind drops it, as do others sharing its loot. **New item...** makes a new
     item right there ("Wolf Pelt").
   - **Explore a place** -- pick a place, or point at the spot in the 3D view and click **Nearest to my
     cursor**. Places are the area triggers the game client knows (new ones need a client patch); places
     used by another quest or for teleports and inns are greyed out.
   - **Reach a reputation** -- "Become Honored with Stormwind".

   No objectives: players just go and talk to the NPC the quest is handed in to.
4. **Rewards** -- experience (**Use typical** fills in what the game's quests of that level give), money,
   items always given, items the player picks one of, reputation with up to five factions, a spell taught
   and a spell cast on the player.
5. **Givers** -- the NPCs or **objects** (a wanted poster) that give the quest and that it is handed in to
   (NPCs become quest givers automatically), an **item that starts the quest** ("This Item Begins a Quest";
   your own items only), an item handed out when accepting ("Here, take this bottle"), and the quest offered
   right after this one is handed in.
6. **Events** -- what happens when the quest is accepted and when it is handed in, as a timeline: the NPC
   **says** or **yells** something, does an **emote**, **casts a spell** on the player, the player **gets an
   item**, a creature **appears** (at your cursor position, for a number of seconds), the NPC **attacks** the
   player, or the quest is **completed** -- for "listen to the story" quests: the NPC talks, then completes
   the quest for the player. Each event happens a number of seconds after the previous one.

   Escorts and other complex scripts are not supported. If a quest already has scripted steps the editor
   cannot show, the page says so; changing the events there replaces all of them.

### Items

**New item...** (in the quest editor) and **Your items...** (in the quest list) open the item editor: name,
kind (quest item, trade goods, junk, consumable), quality, the item it **looks like** (its icon), a flavour
text, stack size and vendor price, with a live preview of its tooltip. Items can be deleted once no quest uses
them.

## Quest chains

In the quest list, select a quest and click **Chain...** to see its whole chain as a diagram, left to right in
the order players do it (with nothing selected: every chain your quests are in).

- **Link two quests**: drag from the **●** on a quest's right edge onto another quest. If that quest
  already needs another one, you choose: players need **all of them**, **any one of them**, or **only** the
  new one.
- **Arrows**: solid = offered right after the previous quest; dashed = only unlocked by it. **ALL** / **ANY**
  tags show quests that need several others. Right-click an arrow to switch these or remove the link.
- **Either / or**: right-click a quest → **Only one of this and...** to make quests close each other ("choose
  a side"). They get the same coloured **EITHER/OR** tag.
- **Right-click a quest** also edits it, makes a **new quest after this one** (given by whoever takes the
  first one back), or hides it.
- **Add a quest** (top) brings another quest and its chain into view; **Tidy up** lays everything out again.
- Link changes are saved together with **Save changes**.

Limits (the database's): a quest unlocks at most **one** quest when several quests are needed together, and
belongs to at most one either/or or all-of group. Only your own quests can be linked to need several quests;
one earlier quest can be any quest, the game's included.

## After saving

**Restart the world server** (`mangosd` / `worldserver`): it reads NPCs, quests and items only at startup.

Every save also writes a re-runnable copy to the project folder, in `sql_exports/npcs/`, `sql_exports/quests/`
and `sql_exports/items/` (deleting removes it). These files can be run any number of times and recreate the
NPC, quest or item exactly as saved. Keep them: if the world database is ever rebuilt, running them puts your
content back.

The files match the database they were saved from. Files from a vmangos database do **not** work on a
Turtle / tortoise-wow database (different columns, and an NPC made on vmangos with an ID like 90974 would
replace one of the game's NPCs there); remake the content with the editors on the new server instead.

**vmangos Docker setups** (`vmangos-deploy`): automatic world database corrections re-create the world
database now and then, which removes custom content. Copy the files from `sql_exports/` into
`storage/database/custom-sql/`; the server applies them on every start.
