#!/usr/bin/env python3
"""TCP, HTTP and scripts (0.29.0): ManiOS fetches files from a web server
on the host through QEMU's user network, byte for byte; HTTP errors and a
refused connection are reported; #! scripts run with their arguments; a
dosrun launcher runs a DOS program.

Usage: tests/net_tcp_test.py [path-to-kernel-elf]
"""
import http.server
import os
import socketserver
import subprocess
import sys
import tempfile
import threading

from console_test import SHELL_PROMPT, Machine, TestFailure, run_command
from dosrun_test import build_disk, hello_com, mt, put

PORT = 18123


def serve(root):
    class Handler(http.server.SimpleHTTPRequestHandler):
        def __init__(self, *a, **k):
            super().__init__(*a, directory=root, **k)

        def log_message(self, *a):
            pass

    socketserver.TCPServer.allow_reuse_address = True
    srv = socketserver.ThreadingTCPServer(("0.0.0.0", PORT), Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv


def main():
    kernel = sys.argv[1] if len(sys.argv) > 1 else "build/manios-zkt.elf"
    work = tempfile.mkdtemp(prefix="zkt-tcp-")
    www = os.path.join(work, "www")
    os.mkdir(www)
    big = os.urandom(256 * 1024)
    with open(os.path.join(www, "big.bin"), "wb") as f:
        f.write(big)
    with open(os.path.join(www, "hello.txt"), "w") as f:
        f.write("hello over tcp\n")
    disk = os.path.join(work, "disk.img")
    src = build_disk(disk)
    mt(disk, "mdel", "::*")
    put(disk, "HELLO.COM", hello_com(), src)
    put(disk, "ECHOARGS", b"#!/bin/echo first\n", src)
    put(disk, "SAYHI", b"#!/bin/dosrun --launcher\nprogram /n/ata0p1/HELLO.COM\n", src)
    # A FAT12 volume (2047 clusters of 8 sectors, mformat's choice for a
    # 15 MiB partition): files there must span many clusters.
    fat12 = os.path.join(work, "fat12.img")
    with open(fat12, "wb") as f:
        f.truncate(16 * 1024 * 1024)
    subprocess.run(["mformat", "-i", fat12, "-T", "30720", "-h", "16", "-s", "63", "-c", "8",
                    "-v", "FAT12", "::"], check=True, capture_output=True)
    if "FAT12" not in subprocess.run(["minfo", "-i", fat12], capture_output=True,
                                     text=True).stdout:
        sys.exit("net_tcp_test: mformat did not make a FAT12 volume")
    srv = serve(www)
    failures = 0

    def step(name, fn):
        nonlocal failures
        try:
            fn()
            print(f"PASS: {name}")
        except TestFailure as e:
            print(f"FAIL: {name}: {e}")
            failures += 1

    m = Machine(kernel, ["-drive", f"file={disk},format=raw,if=ide",
                         "-drive", f"file={fat12},format=raw,if=ide",
                         "-nic", "user,model=e1000"], memory=64)
    try:
        m.expect(SHELL_PROMPT, timeout=90)
        step("hget: a small file over TCP from the host",
             lambda: run_command(m, "serial", f"hget http://10.0.2.2:{PORT}/hello.txt",
                                 ["hello over tcp"], SHELL_PROMPT))
        step("hget: 256 KiB to the FAT disk",
             lambda: run_command(m, "serial",
                                 f"hget http://10.0.2.2:{PORT}/big.bin /n/ata0p1/BIG.BIN",
                                 ["262144 bytes"], SHELL_PROMPT))
        step("hget: 256 KiB onto a FAT12 volume (many 4 KiB clusters)",
             lambda: run_command(m, "serial",
                                 f"hget http://10.0.2.2:{PORT}/big.bin /n/ata1/BIG.BIN",
                                 ["262144 bytes"], SHELL_PROMPT))
        step("hget: HTTP 404 is an error",
             lambda: run_command(m, "serial", f"hget http://10.0.2.2:{PORT}/none",
                                 ["HTTP 404"], SHELL_PROMPT))
        step("hget: a closed port is refused",
             lambda: run_command(m, "serial", "hget http://10.0.2.2:1/",
                                 ["connection refused"], SHELL_PROMPT))
        step("#! script: interpreter, its argument, the script, then the arguments",
             lambda: run_command(m, "serial", "/n/ata0p1/echoargs second",
                                 ["first /n/ata0p1/echoargs second"], SHELL_PROMPT))
        step("dosrun launcher script runs the DOS program it names",
             lambda: run_command(m, "serial", "/n/ata0p1/sayhi",
                                 ["Hello from DOS!"], SHELL_PROMPT))
    finally:
        m.close()
        srv.shutdown()
    got = subprocess.run(["mcopy", "-n", "-i", f"{disk}@@{2048 * 512}", "::/BIG.BIN", "-"],
                         capture_output=True).stdout
    got12 = subprocess.run(["mcopy", "-n", "-i", fat12, "::/BIG.BIN", "-"],
                           capture_output=True).stdout
    for name, data in (("FAT16", got), ("FAT12", got12)):
        if data == big:
            print(f"PASS: the 256 KiB file arrived byte for byte ({name})")
        else:
            print(f"FAIL: the file differs on {name} ({len(data)} bytes)")
            failures += 1
    fsck = subprocess.run(["mdir", "-i", fat12, "::"], capture_output=True, text=True)
    if fsck.returncode:
        print(f"FAIL: the FAT12 volume no longer reads: {fsck.stderr}")
        failures += 1
    print(f"{'OK' if not failures else 'FAILED'}: {failures} failure(s)")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
