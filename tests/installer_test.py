#!/usr/bin/env python3
"""Installer profiles (0.30.0, ADR-0011), on QEMU's BIOS from the ISO:

  - install --profile server writes the base system (BASE.BIN from the
    CD), a FAT16 data partition holding SETUP.CFG, and rc=/boot/etc/rc.setup;
    the host's mtools and fsck.fat read the partition;
  - the installed disk boots on its own: setup finishes (no packages, no
    restart), the host name is set, the data partition has home/ and
    SETUP.DON, and there is no ManiDE or ManiDOS;
  - a profile with packages and --repo cd on a CD without REPO is refused;
  - mkfat (base system only) formats a region of a disk that the host's
    tools then read.

Packages from the CD's signed repository need the repository key, so
that part is tested outside the tree (retest-028/e2e_install.py).

Usage: tests/installer_test.py [build-directory]
"""
import os
import shutil
import struct
import subprocess
import sys
import tempfile

from console_test import SHELL_PROMPT, Machine, TestFailure, run_command

FSCK = shutil.which("fsck.fat") or "/usr/sbin/fsck.fat"


def blank(path, mib):
    with open(path, "wb") as f:
        f.truncate(mib << 20)
    return path


def part2(disk):
    with open(disk, "rb") as f:
        mbr = f.read(512)
    ptype = mbr[446 + 16 + 4]
    start, count = struct.unpack_from("<II", mbr, 446 + 16 + 8)
    return ptype, start, count


def main():
    build = sys.argv[1] if len(sys.argv) > 1 else "build"
    iso = os.path.join(build, "manios.iso")
    work = tempfile.mkdtemp(prefix="zkt-installer-")
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

    disk = blank(os.path.join(work, "server.img"), 48)
    other = blank(os.path.join(work, "other.img"), 16)
    m = Machine(None, ["-boot", "d", "-cdrom", iso, "-drive", f"file={disk},format=raw,if=ide",
                       "-drive", f"file={other},format=raw,if=ide"], 64)
    try:
        m.expect(SHELL_PROMPT, timeout=120)

        def refused():
            run_command(m, "serial", "install -y --profile desktop --repo cd ata0",
                        ["no install CD with a REPO"], SHELL_PROMPT)
            run_command(m, "serial", "install -y --profile nosuch ata0", ["no such profile"],
                        SHELL_PROMPT)
        step("refusals: --repo cd without a REPO, an unknown profile", refused)

        def install():
            m.type_serial("install -y --profile server --hostname srv ata0\r")
            start = len(m.output)
            m.expect("is installed on ata0", timeout=120)
            m.expect(SHELL_PROMPT, timeout=30)
            out = m.output[start:].decode(errors="replace")
            for s in ("Server: the shell", "BASE.BIN", "data:      FAT16, 45 MiB (ata0p2)",
                      '"sysname=srv rc=/boot/etc/rc.setup"'):
                need(s in out, f"install output lacks {s!r}: {out[-1500:]}")
        step("install --profile server: the base system and a data partition", install)
    finally:
        m.close()

    def data_partition():
        # (other.img gets its file system from mkfat on the installed
        # system below; checked after that.)
        ptype, start, count = part2(disk)
        need(ptype == 0x06 and start % 2048 == 0, f"partition 2: type {ptype:#x} at {start}")
        img = f"{disk}@@{start * 512}"
        listing = subprocess.run(["mdir", "-b", "-i", img, "::"], capture_output=True, text=True)
        need("SETUP.CFG" in listing.stdout.upper(), listing.stdout + listing.stderr)
        cfg = subprocess.run(["mtype", "-i", img, "::SETUP.CFG"], capture_output=True, text=True).stdout
        lines = [l.strip() for l in cfg.splitlines()]
        need("profile=server" in lines and "packages=" in lines, cfg)
        raw = os.path.join(work, "p2.img")
        with open(disk, "rb") as f, open(raw, "wb") as o:
            f.seek(start * 512)
            o.write(f.read(count * 512))
        fsck = subprocess.run([FSCK, "-n", raw], capture_output=True, text=True)
        need(fsck.returncode == 0, fsck.stdout + fsck.stderr)
    step("the host's mtools and fsck.fat read the data partition", data_partition)

    m = Machine(None, ["-boot", "c", "-drive", f"file={disk},format=raw,if=ide",
                       "-drive", f"file={other},format=raw,if=ide"], 64)
    try:
        def first_boot():
            out = m.expect(SHELL_PROMPT, timeout=120)
            need("setup: finishing the installation on ata0p2 (profile server)" in out
                 and "setup: done" in out, out[-1500:])
            run_command(m, "serial", "cat /dev/sysname", ["srv"], SHELL_PROMPT)
            out = run_command(m, "serial", "ls /n/ata0p2", ["home"], SHELL_PROMPT)
            need("setup.don" in out.lower() and "setup.cfg" not in out.lower(), out)
            run_command(m, "serial", "dosrun", ["not found"], SHELL_PROMPT)
            run_command(m, "serial", "manide", ["not found"], SHELL_PROMPT)
        step("the installed server boots; setup finishes once", first_boot)

        def mkfat():
            run_command(m, "serial", "mkfat -y -l SPARE ata1 2048 30720", ["mkfat: done"], SHELL_PROMPT)
        step("mkfat (base system) formats part of a disk", mkfat)
    finally:
        m.close()
    m = Machine(None, ["-boot", "c", "-drive", f"file={disk},format=raw,if=ide"], 64)
    try:
        def second_boot():
            out = m.expect(SHELL_PROMPT, timeout=120)
            need("setup: finishing" not in out, "setup ran again")
        step("the second start does not run setup again", second_boot)
    finally:
        m.close()
    def spare():
        raw = os.path.join(work, "spare.img")
        with open(other, "rb") as f, open(raw, "wb") as o:
            f.seek(2048 * 512)
            o.write(f.read(30720 * 512))
        fsck = subprocess.run([FSCK, "-n", raw], capture_output=True, text=True)
        need(fsck.returncode == 0 and "SPARE" in subprocess.run(
            ["mlabel", "-s", "-i", raw, "::"], capture_output=True, text=True).stdout,
             fsck.stdout + fsck.stderr)
    step("fsck.fat reads mkfat's volume", spare)
    shutil.rmtree(work, ignore_errors=True)
    print(f"{'OK' if not failures else 'FAILED'}: {failures} failure(s)")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
