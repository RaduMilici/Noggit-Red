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
def list_nearby_textures(radius: float = 80.0) -> dict:
    """List the distinct terrain texture (.blp) paths already painted on chunks near the camera. This is
    how you discover VALID texture paths to paint with (e.g. grass/dirt/rock). Returns {"paths":[...]}."""
    lua = (
        "local p=camera_pos() local seen={} local out={} local R=%f local s=32.0 "
        "local dx=-R while dx<=R do local dz=-R while dz<=R do "
        "local c=get_chunk(vec(p.x+dx,p.y,p.z+dz)) "
        "if c then local n=c:get_texture_count() for i=0,n-1 do "
        "local ok,f=pcall(function() return c:get_texture(i) end) "
        "if ok and f and not seen[f] then seen[f]=true out[#out+1]=f end end end "
        "dz=dz+s end dx=dx+s end return table.concat(out,'\\n')"
    ) % radius
    r = _send({"cmd": "run_lua", "code": lua})
    if r.get("ok"):
        paths = [p for p in (r.get("result") or "").split("\n") if p]
        return {"ok": True, "count": len(paths), "paths": paths}
    return r


@mcp.tool()
def height_at(x: float, z: float) -> dict:
    """Ground height at a world XZ -> {"ok":true,"height":..}. Use it to sit models on the terrain."""
    return _send({"cmd": "height_at", "x": x, "z": z})


# ----------------------------------------------------------------------------- edits

@mcp.tool()
def place_model(path: str, x: float, y: float, z: float,
                scale: float = 1.0, rotation_degrees: float = 0.0) -> dict:
    """Place one M2 doodad (path from list_nearby_models) at a world coordinate. One undo step, and it
    renders LIVE (this uses the same code path as Noggit's Ctrl+V paste). Returns the new object's uid.
    Tip: set y to height_at(x,z).height so it sits on the ground. rotation_degrees spins it about Y."""
    return _send({"cmd": "place_model", "path": path, "x": x, "y": y, "z": z,
                  "scale": scale, "rotation": rotation_degrees})


@mcp.tool()
def place_wmo(path: str, x: float, y: float, z: float,
              rx: float = 0.0, ry: float = 0.0, rz: float = 0.0) -> dict:
    """Place a WMO (building/structure -- houses, towers, ruins) at a world coordinate. Renders LIVE.
    ry rotates about the vertical axis (degrees). Returns the new object's uid."""
    return _send({"cmd": "place_wmo", "path": path, "x": x, "y": y, "z": z,
                  "rx": rx, "ry": ry, "rz": rz})


@mcp.tool()
def change_terrain(x: float, y: float, z: float, amount: float,
                   radius: float = 40.0, brush_type: int = 2,
                   inner_radius: float = 0.0) -> dict:
    """Raise (amount>0) or lower (amount<0) the terrain around a world point, live. `radius` is the
    footprint; `amount` is roughly how many units the center moves. brush_type falloff: 0=Flat, 1=Linear,
    2=Smooth (default, gentle hills), 6=Gaussian (rounded peaks/mountains). y is ignored (2D footprint).
    Build hills/mountains by stacking several overlapping calls; use negative amount to carve valleys/lakebeds."""
    return _send({"cmd": "change_terrain", "x": x, "y": y, "z": z, "change": amount,
                  "radius": radius, "brush_type": brush_type, "inner_radius": inner_radius})


@mcp.tool()
def blur_terrain(x: float, y: float, z: float, radius: float = 40.0,
                 remain: float = 0.5, brush_type: int = 2) -> dict:
    """Smooth/round terrain around a point, live -- softens cliffs and sharp edges toward the local
    average. `remain` 0..1 is blend strength (higher = stronger smoothing). Run a few passes to melt a
    hard edge. Great for fixing the cliff sides left by change_terrain's Smooth brush."""
    return _send({"cmd": "blur_terrain", "x": x, "y": y, "z": z,
                  "radius": radius, "remain": remain, "brush_type": brush_type})


@mcp.tool()
def flatten_terrain(x: float, z: float, height: float, radius: float = 40.0,
                    remain: float = 1.0, brush_type: int = 0) -> dict:
    """Flatten terrain around (x,z) to a level pad at world y=`height`, live. remain=1.0 fully levels it;
    lower values ease it partway. Use this to make building pads / plazas / roads before placing WMOs."""
    return _send({"cmd": "flatten_terrain", "x": x, "z": z, "height": height,
                  "radius": radius, "remain": remain, "brush_type": brush_type})


@mcp.tool()
def paint_texture(x: float, y: float, z: float, texture: str,
                  strength: float = 1.0, radius: float = 15.0,
                  hardness: float = 0.5, pressure: float = 0.9) -> dict:
    """Paint a terrain texture (.blp path from list_nearby_textures) onto the ground around a point, live.
    strength 0..1 is coverage/opacity, radius the brush size, hardness 0..1 the edge falloff. Layer
    textures by painting grass as a base then dirt/rock on slopes and paths with smaller radius."""
    return _send({"cmd": "paint_texture", "x": x, "y": y, "z": z, "texture": texture,
                  "strength": strength, "radius": radius, "hardness": hardness, "pressure": pressure})


@mcp.tool()
def add_water(x: float, y: float, z: float, height: float,
              radius: float = 40.0, liquid_id: int = 2) -> dict:
    """Add a flat body of water around a world point, live. `height` is the water surface level (world y);
    set it just below the surrounding terrain rim so it reads as a lake/pond. `radius` is the extent.
    liquid_id is a LiquidType.dbc id (2 = ocean/still water by default)."""
    return _send({"cmd": "add_water", "x": x, "y": y, "z": z,
                  "height": height, "radius": radius, "liquid_id": liquid_id})


@mcp.tool()
def move_model(uid: int, x: float, y: float, z: float) -> dict:
    """Move a placed object (by uid, from place_model/query_objects) to a new world position, live.
    Returns the object's new uid/x/y/z/scale."""
    return _send({"cmd": "edit_model", "uid": uid, "x": x, "y": y, "z": z})


@mcp.tool()
def rotate_model(uid: int, ry: float, rx: float = None, rz: float = None) -> dict:
    """Rotate a placed object (by uid) about the vertical axis by ry degrees, live. rx/rz optionally tilt
    it. Only the axes you pass change; the rest are kept."""
    cmd = {"cmd": "edit_model", "uid": uid, "ry": ry}
    if rx is not None:
        cmd["rx"] = rx
    if rz is not None:
        cmd["rz"] = rz
    return _send(cmd)


@mcp.tool()
def scale_model(uid: int, scale: float) -> dict:
    """Set the scale of a placed M2 object (by uid), live. 1.0 = default size."""
    return _send({"cmd": "edit_model", "uid": uid, "scale": scale})


@mcp.tool()
def delete_model(uid: int) -> dict:
    """Delete a placed object (by uid, from place_model/query_objects) from the map, live. One undo step."""
    return _send({"cmd": "delete_model", "uid": uid})


@mcp.tool()
def focus_camera(x: float, y: float, z: float,
                 distance: float = 130.0, pitch: float = 55.0) -> dict:
    """Frame a world point for the user from HIGH ABOVE, looking down, so they can see your work. `distance`
    is how far the eye sits from the target (increase for large scenes -- e.g. 250+ for a whole landscape);
    `pitch` is the look-down angle in degrees (bigger = more top-down)."""
    return _send({"cmd": "focus_camera", "x": x, "y": y, "z": z,
                  "distance": distance, "pitch": pitch})


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
