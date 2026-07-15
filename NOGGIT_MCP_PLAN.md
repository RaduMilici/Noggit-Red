# Noggit MCP — AI-Driven Map Authoring

**Status:** LIVE. Phases 0–1 complete; Phase 3 well underway (live terrain/texture/water/models). See §9 for per-phase status.
**Date:** 2026-07-13 (updated 2026-07-14)

### Current status (2026-07-14)
- ✅ **Phase 0** — in-process TCP bridge (`src/noggit/mcp/McpServer`), `run_lua` on the GUI thread, turn-level undo, `focus_camera`. Committed.
- ✅ **Phase 1** — Python MCP sidecar (`noggit_mcp_server.py`) + query tools (`query_objects`, `height_at`, `camera_position`, `list_nearby_models`, `list_nearby_textures`). Committed.
- 🟢 **BREAKTHROUGH** — MCP placements now render **LIVE** (no save/reload). Root cause was the Lua `add_m2` path omitting `waitForChildrenLoaded`; the dedicated `place_model` command mirrors Ctrl+V paste. This unblocked the whole "watch it live" UX.
- 🟡 **Phase 3** — typed native edit commands landed & verified live: `place_model`, `place_wmo`, `change_terrain` (raise/lower, all falloffs), `paint_texture`, `add_water`. Still TODO: `blur_terrain`, `flatten_terrain`, `set_hole`, `set_area_id`, `move/rotate/scale/delete_model`, `set_vertex_color`.
- 🟡 **Phase 2** — grounding works via *in-world discovery* (`list_nearby_models`/`list_nearby_textures` harvest valid FileKeys from the loaded map). No curated manifest or thumbnails yet (PreviewRenderer thumbnails disabled — offscreen-render crash).
- ⏳ **Near-term queue:** aim `focus_camera` from higher up & pitched down (user feedback); `blur_terrain`/`flatten_terrain` for cliff-free hills and building pads; object-editing tools (move/delete); asset palette manifest so we're not limited to what's already painted/placed.
- ❌ Not started: Phase 4 (mockup ingestion), Phase 5 (heightmap import), Phase 6 (embedded chat panel; procedural scatter/road/river).
**Inspiration:** Epic's "State of Unreal 2026" (Unreal Fest Chicago) MCP demo — an MCP server inside the
editor lets an LLM inspect the scene and drive editor tools from a prompt, grounded on starter assets.
We are building the WoW / Noggit-red equivalent.

---

## 1. Goal & target UX

Give an AI a **prompt** (and optionally a **mockup image** or **grayscale heightmap**) plus a curated
**asset palette**, and have it drive Noggit to build/edit a map. Interaction is **conversational and live**:

- Noggit stays **open and visible**; the user watches edits happen in real time.
- The AI **acts directly** (no per-op approval gate). The user reacts in chat — "good", "add X",
  "remove that", "move it north" — exactly like a normal conversation.
- Every AI turn is **one undo unit**, so a bad step is a single Ctrl+Z (and the AI can self-revert).

This is the "Act, I can undo + converse" model, not a propose→approve model.

---

## 2. Key finding — the command layer already exists

Noggit-red ships a **complete embedded Lua scripting system** (sol2 + Lua, compiled in via
`CMakeLists.txt` FIND_PACKAGE Lua/Sol2). It already wraps almost every edit we need. **We are not
building an editing API from scratch** — only a way to *invoke* it on demand and *observe* results.

Relevant existing code (`src/noggit/scripting/`):

- `script_context` (`script_context.hpp`) — a `sol::state`; `world()` returns the active `World*`;
  auto-loads `scripts/*.lua`. This is where arbitrary Lua can be executed.
- `script_global.cpp` — free functions incl. **`add_m2(filename,pos,scale,rot)`**,
  **`add_wmo(filename,pos,rot)`**, `camera_pos`, `get_area_id`, `vec`, `print`.
- `script_standard_brush.hpp/.cpp` — `standard_brush` maps ~1:1 to `World` edits:
  `change_terrain`, `flatten_terrain`, `blur_terrain`, `paint_texture`, `change_vertex_color`,
  `set_area_id`, `set_hole`, `erase_textures`, `clear_textures`, vertex ops.
- `script_selection.hpp` — `selection(p1,p2)` region object: `chunks()`, `verts()`, `textures()`,
  `models()`, `center/min/max/size()`, `apply()`.
- `script_model.hpp` — `model`: get/set pos/rot/scale, `get_filename`, `replace`, `remove`, `get_uid`.
- `script_chunk / script_vert / script_tex / script_image / script_noise / script_random` — batch and
  procedural primitives.
- All bindings registered in `script_registry.ipp` (`register_functions`).

**Caveat:** scripted edits currently bypass the undo/Action stack (no `beginAction`/`endAction` in
`scripting/`). We must add turn-level action wrapping (see §6).

---

## 3. Architecture

```
Claude Desktop / Claude Code  (the agent + vision; the CHAT surface)
   reads: prompt · mockup image · heightmap · asset palette
        │  MCP (stdio or socket)
Python MCP sidecar  (thin; defines tools; emits Lua or typed RPC)
        │  local TCP / named pipe
NEW: Noggit RPC listener  (small C++ addition)
   - receives on background thread
   - MARSHALS execution onto the Qt GUI thread (QMetaObject::invokeMethod, blocking-queued)
   - run_lua(code)      -> script_context           (reuses ALL existing bindings)
   - typed tools (later)-> World:: wrapped in ActionManager
   - returns result / stdout / errors to the sidecar
        │
Existing Noggit:  World · MapView · script_context · ActionManager
```

The **only genuinely new C++ piece is the RPC listener**. Everything below it exists today.

### Injection strategy (recommended)
Start with a **Lua bridge**: one `run_lua(code)` endpoint. This instantly exposes the entire existing
binding set with minimal C++. Harden the hot tools into typed native `World::` calls (with proper undo)
as they stabilize.

### Transport (recommended)
**Python sidecar** over a local socket to start (fast MCP iteration, no C++ recompiles). Optionally fold
the MCP server in-process later for a single binary.

### Client
**Claude Desktop / Claude Code** as the MCP client to start — zero custom UI, the chat + vision surface,
works today (this is how Epic drove the editor). An embedded chat panel inside Noggit is a later polish.

---

## 4. MCP tool catalog → real Noggit entry points

All entry points below already exist unless marked NEW. Files under `src/noggit/`.

| Tool | Backed by |
| --- | --- |
| `run_lua(code)` | **NEW** endpoint → `script_context` (executes on GUI thread) |
| `place_model(path,pos,rot,scale)` | Lua `add_m2`/`add_wmo` OR `World::addM2AndGetInstance`/`addWMOAndGetInstance` |
| `move_model(uid,pos)` | `World::set_model_pos` |
| `rotate_model(uid,rot)` | `World::set_selected_models_rotation` / `rotate_selected_models` |
| `scale_model(uid,v,mode)` | `World::scale_selected_models` (set/add/mult) |
| `delete_model(uid)` | `World::deleteInstance` / `deleteModelInstance` / `deleteWMOInstance` |
| `snap_to_ground(uid)` | `World::snap_selected_models_to_the_ground` |
| `raise_terrain(pos,delta,radius,brush)` | `World::changeTerrain` (or Lua `standard_brush.change_terrain`) |
| `flatten(pos,radius)` / `blur(pos,radius)` | `World::flattenTerrain` / `blurTerrain` |
| `paint_texture(pos,texture,radius,strength)` | `World::paintTexture` (+ `MapChunk::addTexture`) |
| `erase_textures(pos)` / `set_base_texture` | `World::eraseTextures` / `setBaseTexture` |
| `set_vertex_color(pos,color,radius)` | `World::changeShader` |
| `set_hole(pos,radius,hole)` | `World::setHole` |
| `set_area_id(pos,id,radius)` | `World::setAreaID` |
| `set_water(pos,radius,liquid_id,...)` | `World::paintLiquid` |
| `import_heightmap(image,tile)` | `World::importADTHeightmap` (+ per-chunk `setHeightmapImage`) |
| `query_objects_in_region(pos,radius)` | `World::getObjectsInRange` -> `SceneObject*` list |
| `height_at(x,y)` | `World::get_ground_height` / `GetVertex` |
| `area_at(x,y)` | `World::getAreaID` |
| `get_selection()` | `World::current_selection` / `get_selected_objects` |
| `list_assets(filter)` | `clientData()->listfile()->pathToFileDataIDMap()` (same source as AssetBrowser) |
| `thumbnail(path)` | `ui/tools/PreviewRenderer/` |
| `focus_camera(pos)` | **NEW** thin wrapper on `MapView` camera — so the user SEES the AI's work |
| `undo()` / `redo()` | `ActionManager::undo` / `redo` |
| `save()` | `MapView::save(save_mode::changed)` -> `MapIndex::saveChanged` |

---

## 5. Constraints surfaced by the code audit (design around these)

1. **No headless mode.** GUI must be running (`NoggitApplication` only reads argv[0]; no CLI runner).
   The MCP server drives a *live* Noggit instance in-process — matches the "watch it live" UX and Epic's
   own approach.
2. **Main-thread marshaling (correctness-critical).** All edits touch GL/scene state on the Qt GUI
   thread. The RPC listener receives on a background thread but MUST dispatch execution to the main
   thread (`QMetaObject::invokeMethod`, `BlockingQueuedConnection`) and return the result.
3. **Undo gap.** Lua/scripted edits bypass `ActionManager`. We wrap each AI *turn* in
   `beginAction`/`endAction` (see §6).
4. **Asset identity.** Assets are referenced by `FileKey` (path OR FileDataID). The palette manifest
   stores both so the AI cannot hallucinate a path; `list_assets` is the ground truth.
5. **Live viewport refresh.** Edits must mark tiles changed and trigger a redraw so the user sees them;
   pair with `focus_camera` so edits aren't off-screen.

---

## 6. Undo & "turn" semantics (elevated per UX)

- One **AI turn** (one chat request that may issue many tool calls) = **one `ActionManager` action**.
  Wrap the whole turn: `beginAction(mapView, flags, modality)` ... all edits ... `endAction()`.
- `flags` OR-combine what the turn touched (`eCHUNKS_TERRAIN`, `eCHUNKS_TEXTURE`, `eOBJECTS_ADDED`, ...
  from `Action.hpp`).
- Expose `undo()`/`redo()` as tools so "remove what you just did" is a first-class AI action, in addition
  to the user's own Ctrl+Z.
- Optional: snapshot dirty tiles before a large turn for a coarse "revert whole build" safety net.

---

## 7. Input modes

1. **Text prompts** (baseline, always on) — natural language → plan → tool calls.
2. **Mockup images** — vision model reads a top-down sketch/screenshot → spatial plan. Requires a fixed
   **coordinate convention** (e.g. mockup top-left pixel = a chosen tile's NW corner; define pixels→yards
   scale). Needs `list_assets` + thumbnails so "that blob = a tower" resolves to a real asset.
3. **Grayscale heightmaps** — deterministic terrain via `World::importADTHeightmap` / per-chunk
   `setHeightmapImage`; the AI then does the decoration pass (props, textures, water) on top.

---

## 8. Asset catalog / grounding (the #1 quality lever)

Epic stressed: point the model at existing, working assets. Build a **per-project palette manifest**:

- `{ path, FileDataID, type(M2/WMO), bbox size, category/tags, thumbnail }`.
- Source: `clientData()->listfile()->pathToFileDataIDMap()` + AssetBrowser's project-folder recursion.
- Thumbnails: batch-render via `PreviewRenderer`, optionally auto-caption once with the vision model.
- The user hands the AI a curated subset per project ("use only these"), preventing off-theme placements.

---

## 9. Phase plan (revised for the conversational + undo UX)

- ✅ **Phase 0 — RPC listener + `run_lua` + turn-level undo.** DONE. In-process TCP bridge, Lua on the
  GUI thread bracketed in `beginAction`/`endAction`, return channel, viewport refresh + `focus_camera`.
- ✅ **Phase 1 — Python MCP sidecar** wrapping Phase 0. DONE. Query tools live: `query_objects`,
  `height_at`, `camera_position`, `list_nearby_models`, `list_nearby_textures`.
- 🟡 **Phase 2 — Asset catalog + thumbnails + per-project palette.** PARTIAL. In-world discovery works;
  no curated manifest/thumbnails yet (PreviewRenderer offscreen render crashes → thumbnails disabled).
- 🟡 **Phase 3 — Full terrain/texture/water/hole/area tools.** IN PROGRESS. Live & verified:
  `place_model`, `place_wmo`, `change_terrain`, `paint_texture`, `add_water`. TODO: blur/flatten,
  hole, area-id, move/rotate/scale/delete model, vertex color.
- ❌ **Phase 4 — Mockup ingestion**: vision → spatial plan → pixel→world coordinate mapping.
- ❌ **Phase 5 — Heightmap import mode** on `importADTHeightmap`, AI decorates on top.
- 🟡 **Phase 6 (optional)** — typed native tools for hot paths (undo-clean): STARTED EARLY — the Phase 3
  commands are already native/typed, not Lua. Remaining: procedural primitives (scatter/road/river),
  embedded in-Noggit chat panel.

---

## 10. Open decisions

- **A. Lua bridge vs native typed tools** — recommend Lua bridge first, harden hot paths to native later.
- **B. Sidecar vs in-process MCP** — recommend Python sidecar first, fold in-process later.
- **C. Coordinate convention for mockups** — must be pinned before Phase 4.
- **D. Where the chat lives** — Claude Desktop/Code first; embedded Noggit panel is Phase 6 polish.
