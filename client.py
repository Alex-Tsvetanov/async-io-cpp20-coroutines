#!/usr/bin/env python3
"""client.py - a stand-in for nc, so the live demo needs nothing installed.

    python client.py 9009                          send "hello coroutines" once
    python client.py 9009 "hello" "again" "bye"    several messages, one connection

Every message goes over the SAME connection, one after another. Run the
server with --trace and each message shows up as one "resumed" line on the
completion thread, with the same buffer address every time. That is the
coroutine frame, visible.

Windows has no nc by default. This is twenty lines of standard library that
the audience can read, which is better than a binary nobody can.
"""
import socket
import sys

port = int(sys.argv[1]) if len(sys.argv) > 1 else 9009
messages = sys.argv[2:] or ["hello coroutines"]

with socket.create_connection(("127.0.0.1", port), timeout=5) as s:
    for text in messages:
        data = text.encode()
        s.sendall(data)
        got = b""
        while len(got) < len(data):
            chunk = s.recv(65536)
            if not chunk:
                break
            got += chunk
        print(f"sent {text!r:<24} got back {got.decode(errors='replace')!r}")
