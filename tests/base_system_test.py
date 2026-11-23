#!/usr/bin/env python3
"""The base system (0.30.0, ADR-0011): build/base/manios-zkt.elf boots
to the shell without ManiDE and ManiDOS, keeps pkg and install, and
suggests pkg instead of the programs it lacks. The ISO carries its boot
area as BASE.BIN. (Installing manidos onto it needs the signed
repository: tests outside the tree do that.)

Usage: tests/base_system_test.py [build-dir]
"""
import subprocess
import sys
from pathlib import Path

from console_test import SHELL_PROMPT, Machine, TestFailure, run_command

GONE = ("dosrun", "dos", "manide", "desktop", "welcome", "term")
KEPT = ("sh", "pkg", "install", "hget", "ls")


def main():
    build = Path(sys.argv[1] if len(sys.argv) > 1 else "build")
    failures = 0

    def step(name, fn):
        nonlocal failures
        try:
            fn()
            print(f"PASS: {name}")
        except TestFailure as e:
            print(f"FAIL: {name}: {e}")
            failures += 1

    names = set(subprocess.run(["tar", "tf", build / "base/bootfs.tar"], capture_output=True,
                               text=True, check=True).stdout.split())
    full = set(subprocess.run(["tar", "tf", build / "bootfs.tar"], capture_output=True,
                              text=True, check=True).stdout.split())

    def archive():
        missing = [p for p in KEPT if f"bin/{p}" not in names]
        extra = [p for p in GONE if f"bin/{p}" in names]
        if missing or extra or "autoexec.bat" in names or "etc/manide" in names:
            raise TestFailure(f"missing {missing}, extra {extra}")
        if not names < full:
            raise TestFailure("the base archive has files the full one lacks")
    step("base archive: the full one without ManiDE and ManiDOS", archive)

    def on_iso():
        iso = (build / "manios.iso").read_bytes()
        base = (build / "base/manios-base.bin").read_bytes()
        if base not in iso:
            raise TestFailure("BASE.BIN is not on the ISO")
    step("the ISO carries BASE.BIN", on_iso)

    m = Machine(str(build / "base/manios-zkt.elf"), [], memory=48)
    try:
        m.expect(SHELL_PROMPT, timeout=90)
        step("boots; the ready message suggests pkg",
             lambda: None if b"Try ls /bin, help, fetch, pkg." in m.output
             else (_ for _ in ()).throw(TestFailure("ready message")))
        step("ManiDOS is not there",
             lambda: run_command(m, "serial", "dosrun", ["not found"], SHELL_PROMPT))
        step("ManiDE is not there",
             lambda: run_command(m, "serial", "manide", ["not found"], SHELL_PROMPT))
        step("pkg is there",
             lambda: run_command(m, "serial", "pkg", ["usage: pkg"], SHELL_PROMPT))
    finally:
        m.close()
    print(f"{'OK' if not failures else 'FAILED'}: {failures} failure(s)")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
