#!/usr/bin/env python3
# Stand-in for the OpenSSH client used by ssh_tunnel_tests. It never contacts a server: depending on
# the scripted mode it either listens on the requested -L 127.0.0.1 port (a "working tunnel") or prints
# the same stderr text real OpenSSH prints for a failure and exits 255.
#
#   FAKE_SSH_PLAN  file with one mode per line; invocation N uses line N (the last line repeats)
#   FAKE_SSH_LOG   each invocation appends one JSON line {"pid":..., "args":[...]}
#
# Modes: ok | die:<ms> | auth | hostkey_changed | hostkey_unknown | bind | unreachable | hang
import json
import os
import signal
import socket
import sys
import time


def next_mode():
    plan_path = os.environ["FAKE_SSH_PLAN"]
    counter_path = plan_path + ".count"
    with open(plan_path) as f:
        modes = [line.strip() for line in f if line.strip()]
    count = 0
    if os.path.exists(counter_path):
        with open(counter_path) as f:
            count = int(f.read() or 0)
    with open(counter_path, "w") as f:
        f.write(str(count + 1))
    return modes[min(count, len(modes) - 1)]


def local_port(args):
    spec = args[args.index("-L") + 1]  # 127.0.0.1:PORT:HOST:PORT
    bind, port = spec.split(":")[0:2]
    assert bind == "127.0.0.1", spec
    return int(port)


def fail(message):
    sys.stderr.write(message + "\n")
    sys.stderr.flush()
    sys.exit(255)


def serve(port, die_after_ms=None):
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        server.bind(("127.0.0.1", port))
    except OSError:
        fail("bind [127.0.0.1]:%d: Address already in use\n"
             "channel_setup_fwd_listener_tcpip: cannot listen to port: %d\n"
             "Could not request local forwarding." % (port, port))
    server.listen(8)
    server.settimeout(0.05)
    deadline = None if die_after_ms is None else time.time() + die_after_ms / 1000.0
    while True:
        if deadline is not None and time.time() >= deadline:
            fail("client_loop: send disconnect: Broken pipe")
        try:
            conn, _ = server.accept()
            conn.close()
        except socket.timeout:
            pass


def main():
    args = sys.argv[1:]
    with open(os.environ["FAKE_SSH_LOG"], "a") as log:
        log.write(json.dumps({"pid": os.getpid(), "args": args}) + "\n")

    signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
    mode = next_mode()
    host = args[-1]

    if mode == "ok":
        serve(local_port(args))
    elif mode.startswith("die:"):
        serve(local_port(args), int(mode.split(":")[1]))
    elif mode == "auth":
        fail("tester@%s: Permission denied (publickey)." % host)
    elif mode == "hostkey_changed":
        fail("@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\n"
             "@    WARNING: REMOTE HOST IDENTIFICATION HAS CHANGED!     @\n"
             "@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\n"
             "IT IS POSSIBLE THAT SOMEONE IS DOING SOMETHING NASTY!\n"
             "Host key verification failed.")
    elif mode == "hostkey_unknown":
        fail("No ED25519 host key is known for %s and you have requested strict checking.\n"
             "Host key verification failed." % host)
    elif mode == "bind":
        port = local_port(args)
        fail("bind [127.0.0.1]:%d: Address already in use\n"
             "channel_setup_fwd_listener_tcpip: cannot listen to port: %d\n"
             "Could not request local forwarding." % (port, port))
    elif mode == "unreachable":
        fail("ssh: connect to host %s port 22: Connection refused" % host)
    elif mode == "hang":
        while True:
            time.sleep(1)
    else:
        fail("fake_ssh: unknown mode " + mode)


if __name__ == "__main__":
    main()
