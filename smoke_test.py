#!/usr/bin/env python3
"""Smoke test for the coroutine echo demo.

Start the server, then:  python smoke_test.py [port]

Four checks, because four things can break independently:
  small payload   - the happy path
  10 KB payload   - forces the send loop to go round more than once
  20 sequential   - frames are actually destroyed, not leaked into each other
  100 concurrent  - the completion loop does not serialise or drop handles

Exits non-zero on the first failure so it can gate a demo rehearsal.
"""
import re
import socket
import sys
import threading
import time

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 9009
CONCURRENCY = int(sys.argv[2]) if len(sys.argv) > 2 else None
failures = []


def roundtrip(payload, timeout=10, barrier=None):
    s = socket.create_connection(("127.0.0.1", PORT), timeout=timeout)
    try:
        if barrier is not None:
            # Hold every client connected and idle until all are, so the
            # server has a receive posted and waiting on each one. That is
            # what load looks like: operations in flight while the kernel
            # holds the buffer. Then everyone sends at once.
            barrier.wait()
        s.sendall(payload)
        got = b""
        while len(got) < len(payload):
            chunk = s.recv(65536)
            if not chunk:
                break
            got += chunk
        return got
    finally:
        s.close()


def check(name, condition):
    print(f"  {name:<22} {'OK' if condition else 'FAIL'}")
    if not condition:
        failures.append(name)


def concurrent(n):
    """n clients at once, each with its own recognisable payload.

    All n connect first and sit idle, then all send at once (see roundtrip).

    A wrong answer is classified, because the difference is the point of the
    naive demo: "got another client's bytes" is a buffer shared between two
    in-flight operations, not a flaky network.
    """
    foreign, other = [], []
    barrier = threading.Barrier(n)

    def worker(i):
        payload = (b"client-%04d-" % i) * 64
        try:
            got = roundtrip(payload, timeout=5, barrier=barrier)
            if got == payload:
                return
            ids = {int(x) for x in re.findall(rb"client-(\d{4})-", got)}
            (foreign if ids - {i} else other).append(i)
        except Exception:  # noqa: BLE001 - a hang or reset is still a failure
            other.append(i)

    t0 = time.time()
    threads = [threading.Thread(target=worker, args=(i,)) for i in range(n)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    ok = n - len(foreign) - len(other)
    print(f"  ({n} connections in {time.time() - t0:.2f}s: {ok} correct, "
          f"{len(foreign)} got another client's bytes, {len(other)} other failures)")
    return ok == n


# `smoke_test.py PORT N` runs only the concurrency check, at N clients. That is
# the stage version: N=1 then N=100, against echo and then against naive.
if CONCURRENCY is not None:
    print(f"concurrency check, port {PORT}")
    passed = concurrent(CONCURRENCY)
    print("\nall correct" if passed else "\nFAILED")
    sys.exit(0 if passed else 1)

print(f"echo demo smoke test, port {PORT}")

msg = b"hello coroutines"
check("small payload", roundtrip(msg) == msg)

big = bytes(range(256)) * 40  # 10240 bytes
check("10 KB payload", roundtrip(big) == big)

check("20 sequential", all(roundtrip(b"conn-%d" % i) == b"conn-%d" % i
                           for i in range(20)))

check("100 concurrent", concurrent(100))

if failures:
    print(f"\nFAILED: {', '.join(failures)}")
    sys.exit(1)
print("\nall checks passed")
