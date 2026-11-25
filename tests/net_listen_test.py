#!/usr/bin/env python3
"""/dev/random and TCP listening (0.31.0): a ManiOS program listens on a port and the
host connects to it through QEMU's user network (hostfwd):

  - a second announce of the same port is refused (EADDRINUSE), and
    accept on a descriptor that doesn't listen (EINVAL);
  - a connection is accepted, says where it comes from, and echoes
    256 KiB back in capitals (flow control both ways), then the server's
    own close reaches the host;
  - a second client that connects while the first is served waits in the
    backlog and is served next;
  - after the listener is closed, the port takes no connections.

Usage: tests/net_listen_test.py [path-to-kernel-elf]
"""
import os
import re
import socket
import sys
import tempfile
import threading
import time

from console_test import SHELL_PROMPT, Machine, TestFailure, run_command
from dosrun_test import build_disk, mt, put

HOST_PORT, GUEST_PORT = 18321, 8080


def talk(data, results, key, delay=0.0):
    try:
        time.sleep(delay)
        s = socket.create_connection(("127.0.0.1", HOST_PORT), timeout=60)
        got = bytearray()

        def reader():
            while True:
                b = s.recv(65536)
                if not b:
                    return
                got.extend(b)
        t = threading.Thread(target=reader)
        t.start()
        for i in range(0, len(data), 8192):
            s.sendall(data[i:i + 8192])
        s.shutdown(socket.SHUT_WR)
        t.join(60)
        s.close()
        results[key] = bytes(got)
    except OSError as e:
        results[key] = e


def main():
    kernel = sys.argv[1] if len(sys.argv) > 1 else "build/manios-zkt.elf"
    work = tempfile.mkdtemp(prefix="zkt-listen-")
    disk = os.path.join(work, "disk.img")
    src = build_disk(disk)
    ltest = os.path.join(os.path.dirname(os.path.abspath(kernel)), "bootfs", "test", "ltest")
    put(disk, "LTEST", open(ltest, "rb").read(), src)  # a test program, not in the boot archive
    failures = 0

    def step(name, fn):
        nonlocal failures
        try:
            fn()
            print(f"PASS: {name}")
        except TestFailure as e:
            print(f"FAIL: {name}: {e}")
            failures += 1

    def need(cond, msg):
        if not cond:
            raise TestFailure(msg)

    m = Machine(kernel, ["-drive", f"file={disk},format=raw,if=ide",
                         "-nic", f"user,model=e1000,hostfwd=tcp:127.0.0.1:{HOST_PORT}-:{GUEST_PORT}"],
                memory=48)
    try:
        m.expect(SHELL_PROMPT, timeout=90)
        time.sleep(3)  # DHCP
        def random():
            out = run_command(m, "serial", "/n/ata0p1/LTEST -r", ["(16)"], SHELL_PROMPT)
            rows = re.findall(r"random \d:((?: [0-9a-f]{2}){16}) \(16\)", out)
            need(len(rows) == 2 and rows[0] != rows[1], f"/dev/random: {out!r}")
            need(all(len(set(r.split())) > 8 for r in rows), f"/dev/random looks poor: {rows}")
        step("/dev/random: 16 bytes a read, never the same twice", random)

        m.type_serial(f"/n/ata0p1/LTEST {GUEST_PORT} 2\r")
        m.expect(f"ltest: listening on {GUEST_PORT}", timeout=20)
        out = m.output.decode(errors="replace")

        def refusals():
            need("second announce: in use" in out, "a second announce of the port was not refused")
            need("non-listener: WRONG" not in out, "accept on a non-listener did not fail with EINVAL")
        step("a port listened on twice: EADDRINUSE; accept on a non-listener: EINVAL", refusals)

        results = {}
        first = bytes((i * 7 + 3) % 251 for i in range(256 * 1024))
        second = b"second client, waiting its turn\n"
        a = threading.Thread(target=talk, args=(first, results, "a"))
        b = threading.Thread(target=talk, args=(second, results, "b", 0.5))
        a.start()
        b.start()
        a.join(120)
        b.join(120)

        def echo():
            got = results.get("a")
            need(isinstance(got, bytes), f"the first client failed: {got!r}")
            want = first.upper() + f"bye {len(first)}\n".encode()
            need(got == want, f"the echo differs: {len(got)} bytes back, {len(want)} wanted")
            m.expect("ltest: connection 1 from 10.0.2.2!", timeout=10)
        step("accept, the peer's address, 256 KiB echoed both ways, the server's close", echo)

        def backlog():
            got = results.get("b")
            need(got == second.upper() + f"bye {len(second)}\n".encode(), f"the second client got {got!r}")
            m.expect("ltest: connection 2 done", timeout=10)
            m.expect("ltest: closed", timeout=10)
            m.expect(SHELL_PROMPT, timeout=10)
        step("a second client waits in the backlog and is served next", backlog)

        def closed():
            # QEMU accepts on the host side, then finds nothing in the guest:
            # the connection ends at once with no data.
            try:
                s = socket.create_connection(("127.0.0.1", HOST_PORT), timeout=10)
                s.settimeout(20)
                data = s.recv(100)
                s.close()
            except OSError:
                data = b""
            need(data == b"", f"a closed listener still answered: {data!r}")
        step("after close the port takes no connections", closed)
    finally:
        m.close()
    print(f"{'OK' if not failures else 'FAILED'}: {failures} failure(s)")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
