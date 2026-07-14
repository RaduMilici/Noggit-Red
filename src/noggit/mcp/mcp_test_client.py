#!/usr/bin/env python3
"""
Phase 0 MCP test client for Noggit.

Talks to the in-process McpServer (QTcpServer on 127.0.0.1, line-delimited JSON).
This is a MANUAL TEST HARNESS, not the real MCP sidecar yet (that is Phase 1).

Protocol: send one compact JSON object + '\n', read one JSON object + '\n' back.

Usage (a map must be open in Noggit):
    python mcp_test_client.py ping
    python mcp_test_client.py height <x> <z>
    python mcp_test_client.py focus  <x> <y> <z>
    python mcp_test_client.py query  <x> <y> <z> [radius]
    python mcp_test_client.py lua    "add_m2('World\\...\\tree.m2', vec(100,50,100), 1.0, vec(0,0,0))"
    python mcp_test_client.py trees  <x> <y> <z>        # demo: scatter 5 trees around a point
    python mcp_test_client.py save
    python mcp_test_client.py undo
    python mcp_test_client.py redo

Env:
    NOGGIT_MCP_PORT   port to connect to (default 8172; must match Noggit)
"""

import json
import os
import socket
import sys

HOST = "127.0.0.1"
PORT = int(os.environ.get("NOGGIT_MCP_PORT", "8172"))

# Set this to a model that actually exists in YOUR project's listfile.
# (Backslashes are doubled automatically when embedded into Lua.)
TREE_M2 = r"World\Azeroth\Elwynn\PassiveDoodads\Trees\ElwynnTree01.m2"


def send(cmd: dict, timeout: float = 30.0) -> dict:
    """Open a short-lived connection, send one command, return the parsed reply."""
    with socket.create_connection((HOST, PORT), timeout=timeout) as s:
        s.sendall((json.dumps(cmd) + "\n").encode("utf-8"))
        buf = b""
        while b"\n" not in buf:
            chunk = s.recv(4096)
            if not chunk:
                break
            buf += chunk
    line = buf.split(b"\n", 1)[0]
    return json.loads(line.decode("utf-8"))


def lua_str(s: str) -> str:
    """Escape a Python string for embedding inside a single-quoted Lua string literal."""
    return s.replace("\\", "\\\\").replace("'", "\\'")


def run_lua(code: str) -> dict:
    return send({"cmd": "run_lua", "code": code})


def place_trees(x: float, y: float, z: float) -> dict:
    """Demo turn: scatter 5 trees in a small cross around (x, y, z) in one undoable action."""
    offsets = [(0, 0), (8, 0), (-8, 0), (0, 8), (0, -8)]
    path = lua_str(TREE_M2)
    lines = [
        f"add_m2('{path}', vec({x + dx}, {y}, {z + dz}), 1.0, vec(0, 0, 0))"
        for dx, dz in offsets
    ]
    return run_lua("\n".join(lines))


def main(argv):
    if not argv:
        print(__doc__)
        return 1

    cmd = argv[0]
    try:
        if cmd == "ping":
            reply = send({"cmd": "ping"})
        elif cmd == "height":
            x, z = float(argv[1]), float(argv[2])
            reply = send({"cmd": "height_at", "x": x, "z": z})
        elif cmd == "focus":
            x, y, z = float(argv[1]), float(argv[2]), float(argv[3])
            reply = send({"cmd": "focus_camera", "x": x, "y": y, "z": z})
        elif cmd == "query":
            x, y, z = float(argv[1]), float(argv[2]), float(argv[3])
            radius = float(argv[4]) if len(argv) > 4 else 50.0
            reply = send({"cmd": "query_objects", "x": x, "y": y, "z": z, "radius": radius})
        elif cmd == "lua":
            reply = run_lua(argv[1])
        elif cmd == "trees":
            x, y, z = float(argv[1]), float(argv[2]), float(argv[3])
            reply = place_trees(x, y, z)
        elif cmd in ("save", "undo", "redo"):
            reply = send({"cmd": cmd})
        else:
            print(f"unknown command: {cmd}\n")
            print(__doc__)
            return 1
    except (ConnectionRefusedError, socket.timeout) as e:
        print(f"[connect] {e} -- is Noggit running with a map open on port {PORT}?")
        return 2
    except (IndexError, ValueError):
        print(f"bad arguments for '{cmd}'\n")
        print(__doc__)
        return 1

    print(json.dumps(reply, indent=2))
    return 0 if reply.get("ok") else 3


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
