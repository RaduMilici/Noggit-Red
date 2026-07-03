# Noggit Turtle — Release Notes

A fork of **Noggit Red** (WotLK 3.3.5a map editor, based on prophecy-rp/noggit-red)
adapted to render **vanilla 1.12 / Turtle WoW** maps correctly and to edit **server
database spawns** (creatures & gameobjects) directly from MySQL.

This document lists every feature and significant change added since the fork point
(`origin/master`).

---

## 1. Vanilla 1.12 / Classic rendering parity

Noggit Red was built for WotLK data; these changes make classic (1.12) and Turtle
content render correctly alongside 3.3.5a.

### Models (M2)
- **Classic M2 loading & animation** — classic bone / animation-block layouts parsed and
  played (skeletal animation, per-instance animation, global sequences).
- **Texture (UV) animations restored** — flowing lava, water, and fire now animate.
  Fixed three separate bugs that froze them: `animTextures` was hardcoded off for classic
  models, the animation clock was gated behind the model-animation toggle, and the
  `m2_vert` shader transposed a column-major texture matrix (discarding the scroll).
- **Classic single-texcoord fix** — classic M2 vertices carry only one UV set; the second
  was reading garbage and stretching multi-texture passes (e.g. light-ray cones).
- **Character equipment fix** — corrected classic character model gloves/equipment.

### World Models (WMO)
- **Interior lighting parity** — MOCV baked vertex lighting and MOHD ambient handled like
  the reference renderer; fixed washed-out interiors and a white-ambient flood that made
  baked shadows disappear (e.g. Timbermaw).
- **Exterior-lit gate** — fixed burnt/over-dark WMO surfaces.
- **Per-batch additive blending** — additive WMO materials (glows, dome/skybox-mimic
  geometry) draw with proper blending instead of flat/opaque.

### Liquids (water / lava / slime)
- **Sharp, animated magma** — removed the down-scaled UV and LOD bias that blurred lava to
  a flat sheet; lava now shows detailed crust and scrolls/cycles frames.
- **Lava flow controls** — tunable magma flow direction & speed (slowed to a natural rate).
- **Correct lava-vs-water classification** — uses authoritative MCNK header flags, with a
  per-tile nibble fallback, plus MH2O variant-liquid-id resolution (slow/fast/wmo ids map
  to their base type) so lava stops rendering as blue water.
- **Liquid texture frame placeholder** — handles the `%d` frame placeholder in
  `LiquidType.dbc` (3.3.5a lava that previously didn't render).
- **Seamless water** — per-tile seams removed by deriving a continuous depth from the
  shared terrain heightmap; reference-style additive water color (no per-chunk shadow);
  shore-to-deep light gradient with deep water going opaque to hide seafloor seams.
- **WMO water overlap dedupe** — stencil "draw water once per pixel" stops stacked
  translucent planes (e.g. Timbermaw) from compounding into dark patches.
- **Slime animation** slowed to match lava.

---

## 2. Atmosphere & post-processing

- **Screen-space bloom** — full bloom pipeline: scene → offscreen FBO → bright-pass
  (luminance threshold) → separable gaussian ping-pong → composite. Terrain/horizon opt out
  so the ground doesn't wash white. Toggle on the view toolbar (**F5**). *(4 new shaders:
  `bloom_quad_vert`, `bloom_bright_frag`, `bloom_blur_frag`, `bloom_composite_frag`.)*
- **Light rays / volumetric light** — masked-additive light-ray cones with scrolling UVs
  and dust-mote particles.
- **Fog improvements** — doodads, smoke/particles, and light-rays are fogged consistently
  with terrain; additive effects fade to black correctly in fog; warmer Timbermaw fog;
  fog no longer renders distant mesh past the view distance (with a "render distant horizon
  backdrop" toggle); interior WMO fog tracks the zone.
- **Point lights** — M2 emitter / synthetic point-light support feeding terrain/WMO/M2.

---

## 3. Zone music

- **In-game-style zone music** — plays the current zone's music like the client, resolving
  through the DBC chain (`AreaTable` / `WMOAreaTable` → `ZoneMusic` → `SoundEntries`),
  day/night aware, random track from the zone playlist, authored silence interval between
  tracks, and continuous playback (auto-advances to the next track).
- **City / WMO-interior music** — whole-WMO fallback so capital cities (e.g. Ironforge)
  resolve their interior music even when the exact group carries none; only applies once
  the camera is genuinely inside the building.
- **Crossfade** — switching zones fades the new track in while the old fades out (overlap),
  using two audio decks (no silent gap).
- **Sound/music toolbar control** — dropdown with enable toggle, volume, and the current
  zone's song list (click a song to play it).
- **Persisted settings** — enable state and volume are saved and restored across sessions.

---

## 4. Creature spawn editor (MySQL)

Edit live server creature spawns from the mangos/vmangos `creature` tables.

- **Load & render** creature spawns for the current map from MySQL (`creature` +
  `creature_template`, with display id, scale, and equipment/attachment models).
- **Creature Browser** dock — searchable list of all spawns, with a **"Zone only"** filter
  that limits the list to the camera's current zone (Searing Gorge, Duskwood, …).
- **Selection markers** — a colored disc under each spawn, shown only while the Creature
  tool is active.
- **Editing** — click to select, Shift box-select, drag to move; a coordinate editor
  (X / Y / Z / orientation) with the true position.
- **NPC Model Picker** — browse `creature_template` (searchable, type filter, faction/rank
  filters), live 3D preview, and create brand-new spawns ("Add Pending Spawn").
- **Delete & undo** — **Del** marks a spawn for deletion (exported as `DELETE`), **Ctrl+Z**
  restores it.
- **Camera focus** — selecting a spawn from the list focuses the camera on it at a 45° angle.
- **Patrol paths** — draws each creature's `creature_movement` route as a colored line
  (per-creature colors, terrain line-of-sight, shown from the creature's render distance);
  toggle in the Creature secondary toolbar. When a creature is selected, only its path shows.
- **SQL export** — writes `INSERT` / `UPDATE` / `DELETE` statements for all pending changes
  to `sql_exports/creature_spawns/`. A "Pending" dropdown lists changes awaiting export.

---

## 5. GameObject spawn editor (MySQL)

The full Creature toolset, mirrored for the `gameobject` tables (mining nodes, chests,
doors, props, …).

- **Load & render** gameobjects for the current map (`gameobject` + `gameobject_template`).
- **GameObject Browser** dock — searchable list with the same **"Zone only"** filter.
- **Selection markers** — a distinct blue/purple disc, shown only in the GameObject tool.
- **Editing** — select / box-select / drag-to-move and a coordinate editor.
- **GameObject Model Picker** — browse `gameobject_template` (searchable, type filter:
  Door / Chest / Goober / …), live preview, and create new spawns.
- **Delete & undo** — **Del** + **Ctrl+Z**, same as creatures.
- **Camera focus** from the list.
- **SQL export** — `INSERT` / `UPDATE` / `DELETE` to `sql_exports/gameobject_spawns/`.

---

## 6. Editor quality-of-life & settings

- **Toolbar** — dedicated Creature and GameObject tools placed side-by-side with unique
  icons.
- **Time-of-day slider** — scrub the in-editor time of day from the toolbar.
- **Animation toggle = pause** — turning model animations off now *freezes* models and
  particles in place (instead of hiding particles or letting NPCs keep animating); liquids
  keep flowing. Toggling back on resumes smoothly.
- **Coordinate editor ranges** — widened so spawns anywhere on the map aren't clamped, and
  orientation is normalized to 0–360° (reads the real DB value instead of pinning at 360).
- **Live Settings sliders** — water opacity, creature render distance, and view/object
  render distance update the scene immediately.
- **Toggles** — WMO water overlap dedupe (stencil), render distant horizon backdrop.

---

## 7. Stability & infrastructure

- **MySQL integration layer** (`src/mysql/`) — connection handling plus spawn / template /
  patrol-path queries used by the editors.
- **Thread-safety fix** — `SceneObject`'s tile list is now guarded by a shared mutex,
  fixing a data-race crash during async tile streaming (concurrent `refTile`/`derefTile`).
- **Crash diagnostics** — Release builds now ship debug symbols and write a symbolicated
  call stack to `log.txt` on a crash (StackWalker output redirected to the log).

---

*Generated from the diff against `origin/master` and the project's development notes.*
