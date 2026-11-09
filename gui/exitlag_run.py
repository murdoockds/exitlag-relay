#!/usr/bin/env python3
"""exitlag-run COMMAND...: start a game with its traffic already relayed.

Use as a Steam launch option: exitlag-run %command%

It asks the relay daemon to move this process into the relay cgroup, then
executes the game, so every socket the game opens is diverted (the daemon's
process watcher can otherwise miss sockets opened in the first moments).
If the daemon isn't reachable the game simply starts normally.
"""
import json
import os
import socket
import sys

CONTROL = os.environ.get("EXITLAG_RELAY_CONTROL", "/run/exitlag-relay/control.sock")

if len(sys.argv) < 2:
    sys.exit(__doc__)
try:
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
        s.settimeout(2)
        s.connect(CONTROL)
        s.sendall(json.dumps({"cmd": "attach", "pid": os.getpid()}).encode() + b"\n")
        resp = json.loads(s.makefile().readline())
        if not resp.get("ok"):
            print("exitlag-run:", resp.get("error"), file=sys.stderr)
except (OSError, ValueError) as e:
    print("exitlag-run: relay daemon not reachable:", e, file=sys.stderr)
os.execvp(sys.argv[1], sys.argv[1:])
