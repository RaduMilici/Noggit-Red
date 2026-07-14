# Noggit MCP — Phase 0

In-process endpoint that lets an external agent drive the Noggit editor. Phase 0 is the plumbing:
a tiny TCP server inside `MapView` that runs Lua against the live scripting engine, moves the camera,
queries the scene, saves, and undoes/redoes — each request handled on the GUI thread. See the full
roadmap in `NOGGIT_MCP_PLAN.md` at the repo root.

## Files

| File | Role |
| --- | --- |
| `McpServer.hpp/.cpp` | The server. `QTcpServer` on `127.0.0.1`, line-delimited JSON. Owned by `MapView`. |
| `mcp_test_client.py` | Manual test harness (stdlib only). Not the real MCP sidecar (that is Phase 1). |

Wiring into the editor (already done, all in `noggit-red-features`):
- `MapView.h` — forward-declares + `friend`s `Noggit::Mcp::McpServer`; owns `std::unique_ptr<McpServer> _mcp_server`.
- `MapView.cpp` — includes the header; constructs the server in `setupScriptingUi()`.
- No CMake edits: `src/noggit/**` is auto-globbed and `Qt5::Network` is already linked; just **re-run CMake configure** so the new files are picked up.

## Build & run

Build the `noggit` target as usual (this is the fresh `noggit-red-features` tree — configure a build dir
first, since the 2 GB `build-msvc9-fresh` was intentionally not copied). Launch Noggit and **open a map** —
the server starts when a map view is created and logs:

```
[MCP] listening on 127.0.0.1:8172
```

Port override: set env `NOGGIT_MCP_PORT` before launching (the client reads the same var).

## Protocol

One JSON object per line in, one JSON object per line out.

```
-> {"cmd":"ping"}
<- {"ok":true,"result":"pong","port":8172}
```

### Commands

| cmd | fields | effect |
| --- | --- | --- |
| `ping` | — | health check |
| `run_lua` | `code` | run a Lua string in the live `script_context`, wrapped in ONE undo action |
| `focus_camera` | `x,y,z` | frame the camera on a world point (loads the tile, aims at it) |
| `query_objects` | `x,y,z,radius?` | list M2/WMO instances near a point (`uid,type,x,y,z,scale`) |
| `height_at` | `x,z` | ground height at a world XZ |
| `save` | — | save changed tiles |
| `undo` / `redo` | — | step the action history |

Every reply has `"ok": true|false`; failures add `"error": "..."`.

### Undo model
`run_lua` wraps the whole call in a single `beginAction`/`endAction`, so one request = one Ctrl-Z. Object
placement via `add_m2`/`add_wmo` registers into the active action too, so placed models are undoable.

## Quick test (map open in Noggit)

```
python mcp_test_client.py ping
python mcp_test_client.py height 100 100          # find ground height at X=100 Z=100
python mcp_test_client.py focus  100 50 100       # look at that spot
python mcp_test_client.py trees  100 50 100       # scatter 5 trees (edit TREE_M2 in the script first!)
python mcp_test_client.py query  100 50 100 60    # confirm they're there
python mcp_test_client.py undo                    # remove them in one step
python mcp_test_client.py save
```

> Set `TREE_M2` in `mcp_test_client.py` to a model path that exists in your project's listfile.

## Known Phase-0 caveats (to validate on first run)
- `run_lua` passes a broad `ActionFlags` set to `beginAction`. If any flag triggers an expensive
  snapshot on begin (rather than lazily per-edit), narrow it or make it a per-request field.
- A long request (e.g. `add_m2` waiting for a model to load) briefly blocks the UI thread — acceptable
  for Phase 0; Phase 6 can move heavy work off the main thread.
- The server binds per open map. Opening a second map while one is open may fail to bind the port
  (logged, harmless).
