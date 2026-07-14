#!/usr/bin/env python3
"""
Noggit MCP sidecar (Phase 1).

An MCP server that Claude Desktop / Claude Code launches over stdio. It forwards each tool call to the
in-process Noggit bridge (McpServer, a line-delimited-JSON TCP server on 127.0.0.1:8172), letting you
drive the open Noggit map conversationally: "plant firs on that ridge", "move it left", "undo that".

Setup:
    pip install mcp        (already done)
    Noggit must be RUNNING with a map open (the bridge starts with the map view).

Register in Claude Desktop (%APPDATA%\\Claude\\claude_desktop_config.json):
    {
      "mcpServers": {
        "noggit": { "command": "py", "args": ["-3", "<ABSOLUTE PATH TO THIS FILE>"] }
      }
    }
Then restart Claude Desktop. (Claude Code: add an equivalent entry to your MCP config.)

Env: NOGGIT_MCP_PORT overrides the bridge port (default 8172; must match Noggit).
"""

import json
import os
import socket

from mcp.server.fastmcp import FastMCP

HOST = "127.0.0.1"
PORT = int(os.environ.get("NOGGIT_MCP_PORT", "8172"))

mcp = FastMCP("noggit")


def _send(cmd: dict, timeout: float = 60.0) -> dict:
    """Send one command to the Noggit bridge and return the parsed JSON reply."""
    try:
        with socket.create_connection((HOST, PORT), timeout=timeout) as s:
            s.sendall((json.dumps(cmd) + "\n").encode("utf-8"))
            buf = b""
            while b"\n" not in buf:
                chunk = s.recv(4096)
                if not chunk:
                    break
                buf += chunk
    except (ConnectionRefusedError, socket.timeout, OSError) as e:
        return {"ok": False, "error": f"cannot reach Noggit bridge on {HOST}:{PORT} ({e}). "
                                       f"Is Noggit running with a map open?"}
    if not buf:
        return {"ok": False, "error": "empty reply from Noggit bridge"}
    return json.loads(buf.split(b"\n", 1)[0].decode("utf-8"))


def _lua_str(s: str) -> str:
    """Escape a string for embedding inside a single-quoted Lua literal."""
    return s.replace("\\", "\\\\").replace("'", "\\'")


# ----------------------------------------------------------------------------- health / raw

@mcp.tool()
def ping() -> dict:
    """Check that the Noggit bridge is alive. Returns {"ok":true,"result":"pong"} when a map is open."""
    return _send({"cmd": "ping"})


@mcp.tool()
def run_lua(code: str) -> dict:
    """Run a raw Lua snippet against the open map (advanced escape hatch). The whole call is ONE undo
    step. If the script ends with `return <value>`, that value comes back as "result". Available Lua:
    camera_pos(), vec(x,y,z), add_m2(path,pos,scale,rot), add_wmo(path,pos,rot), select_origin(pos,r,r)
    -> :models()/:verts(), model:get_filename()/get_pos()/get_pos(), get_area_id(pos), string.*, math.*"""
    return _send({"cmd": "run_lua", "code": code})


# ----------------------------------------------------------------------------- scene discovery

@mcp.tool()
def camera_position() -> dict:
    """Where the user's camera is, in Noggit world coordinates -> {"ok":true,"x":..,"y":..,"z":..}.
    Use this as the reference point for placing things "here" / "in front of me"."""
    r = _send({"cmd": "run_lua",
               "code": "local p=camera_pos() return string.format('%f %f %f', p.x, p.y, p.z)"})
    if r.get("ok") and r.get("result"):
        try:
            x, y, z = (float(v) for v in r["result"].split())
            return {"ok": True, "x": x, "y": y, "z": z}
        except ValueError:
            pass
    return r


@mcp.tool()
def list_nearby_models(radius: float = 200.0) -> dict:
    """List the distinct M2 model paths already placed near the camera. This is how you discover VALID
    asset paths to reuse (the in-editor asset browser is unavailable). Returns {"ok":true,"paths":[...]}."""
    lua = ("local p=camera_pos() local sel=select_origin(p,%f,%f) local seen={} local out={} "
           "for i,m in pairs(sel:models()) do local f=m:get_filename() "
           "if f and not seen[f] then seen[f]=true out[#out+1]=f end end "
           "return table.concat(out,'\\n')") % (radius, radius)
    r = _send({"cmd": "run_lua", "code": lua})
    if r.get("ok"):
        paths = [p for p in (r.get("result") or "").split("\n") if p]
        return {"ok": True, "count": len(paths), "paths": paths}
    return r


@mcp.tool()
def query_objects(x: float, y: float, z: float, radius: float = 50.0) -> dict:
    """List M2/WMO instances within `radius` of a world point. Returns each object's uid, type, x/y/z,
    scale. Use it to see what's there and to verify your own edits (count before/after)."""
    return _send({"cmd": "query_objects", "x": x, "y": y, "z": z, "radius": radius})


@mcp.tool()
def height_at(x: float, z: float) -> dict:
    """Ground height at a world XZ -> {"ok":true,"height":..}. Use it to sit models on the terrain."""
    return _send({"cmd": "height_at", "x": x, "z": z})


# ----------------------------------------------------------------------------- edits

@mcp.tool()
def place_model(path: str, x: float, y: float, z: float,
                scale: float = 1.0, rotation_degrees: float = 0.0) -> dict:
    """Place one M2 doodad (path from list_nearby_models) at a world coordinate. One undo step.
    Tip: set y to height_at(x,z).height so it sits on the ground. rotation_degrees spins it about Y."""
    lua = (f"add_m2('{_lua_str(path)}', vec({x},{y},{z}), {scale}, vec(0,{rotation_degrees},0)) "
           f"return 'placed'")
    return _send({"cmd": "run_lua", "code": lua})


@mcp.tool()
def focus_camera(x: float, y: float, z: float) -> dict:
    """Aim the user's camera at a world point so they can see what you did / where you're working."""
    return _send({"cmd": "focus_camera", "x": x, "y": y, "z": z})


@mcp.tool()
def undo() -> dict:
    """Undo the last edit turn (reverts a whole place/edit in one step)."""
    return _send({"cmd": "undo"})


@mcp.tool()
def redo() -> dict:
    """Redo the last undone edit."""
    return _send({"cmd": "redo"})


@mcp.tool()
def save() -> dict:
    """Save changed tiles to disk. Ask the user before calling this -- it writes their map."""
    return _send({"cmd": "save"})


if __name__ == "__main__":
    mcp.run()
