#!/usr/bin/env python3
# Stand-in for ssh-keyscan used by ssh_tunnel_tests: prints the file named by FAKE_KEYSCAN_OUT
# (known_hosts-format lines) to stdout, like a successful scan. No network access.
import os
import sys

path = os.environ.get("FAKE_KEYSCAN_OUT", "")
if path and os.path.exists(path):
    with open(path) as f:
        sys.stdout.write(f.read())
else:
    sys.stderr.write("fake_keyscan: no keys\n")
