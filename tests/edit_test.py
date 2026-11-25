#!/usr/bin/env python3
"""edit, the text editor package (0.30), and the console's raw mode it
uses (/dev/consctl, con_raw): typed over the serial line and on the
PS/2 keyboard (QEMU's sendkey), each file then read back on the host.

  - a new file: typing, Enter, arrows, Home, ^O save, ^X exit;
  - a DOS file (CR LF) keeps its line ends; End and Delete;
  - ^K cuts lines, ^U pastes them; ^W finds; ^L goes to a line;
  - ^X with changes asks, and "n" leaves the file as it was;
  - the PS/2 keyboard's arrows and Ctrl keys arrive in raw mode;
  - afterwards the console reads lines again (cooked).

Usage: tests/edit_test.py [path-to-kernel-elf]
"""
import os
import subprocess
import sys
import tempfile
import time

from console_test import SHELL_PROMPT, Machine, TestFailure, run_command
from dosrun_test import build_disk, mt, put

ESC = "\x1b"
UP, DOWN, RIGHT, LEFT = ESC + "[A", ESC + "[B", ESC + "[C", ESC + "[D"
HOME, END, DEL = ESC + "[H", ESC + "[F", ESC + "[3~"


def ctrl(c):
    return chr(ord(c) & 0x1F)


def main():
    kernel = sys.argv[1] if len(sys.argv) > 1 else "build/manios-zkt.elf"
    build = os.path.dirname(kernel) or "."
    work = tempfile.mkdtemp(prefix="zkt-edit-")
    disk = os.path.join(work, "disk.img")
    src = build_disk(disk)
    mt(disk, "mdel", "::*")
    put(disk, "EDIT", open(os.path.join(build, "apps", "bin", "edit"), "rb").read(), src)
    put(disk, "DOS.TXT", b"one\r\ntwo\r\nthree\r\n", src)
    put(disk, "ABC.TXT", b"a\nb\nc\n", src)
    put(disk, "KEEP.TXT", b"keep me\n", src)
    put(disk, "KBD.TXT", b"abc\n", src)
    put(disk, "FIND.TXT", b"alpha\nbeta\ngamma target\ndelta\n", src)
    put(disk, "TERM.TXT", b"in a pane\n", src)
    put(disk, "SESSION", b"term /n/ata0p1/edit /n/ata0p1/TERM.TXT\n", src)
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

    m = Machine(kernel, ["-drive", f"file={disk},format=raw,if=ide"], memory=48)

    def keys(text, pause=0.15):
        for chunk in text:
            m.type_serial(chunk)
            time.sleep(pause)

    def session(name, typing, wait_for=None):
        m.type_serial(f"/n/ata0p1/edit /n/ata0p1/{name}\r")
        m.expect(f"edit  /n/ata0p1/{name}", timeout=20)
        time.sleep(0.8)  # the size question times out (no answer on serial)
        keys(typing)
        if wait_for:
            m.expect(wait_for, timeout=10)

    def leave(save=True):
        if save:
            keys([ctrl("o")])
            m.expect("Saved", timeout=10)
        keys([ctrl("x")])
        m.expect(SHELL_PROMPT, timeout=10)

    try:
        m.expect(SHELL_PROMPT, timeout=90)

        def new_file():
            session("NEW.TXT", ["hello world", "\r", "second line", UP, HOME, "X"])
            leave()
        step("a new file: typing, Enter, Up, Home, ^O, ^X", new_file)

        def dos_file():
            session("DOS.TXT", [DOWN, END, "!", DEL])
            leave()
        step("a DOS file: End, Delete joins lines; CR LF kept", dos_file)

        def cut_paste():
            session("ABC.TXT", [ctrl("k"), DOWN, ctrl("u")])
            leave()
        step("^K cuts a line, ^U pastes it", cut_paste)

        def find_goto():
            session("FIND.TXT", [ctrl("w"), "target", "\r", "*", ctrl("l"), "4", "\r", "#"])
            leave()
        step("^W finds, ^L goes to a line", find_goto)

        def asks():
            session("KEEP.TXT", ["zzz", ctrl("x")], "Save the changes first?")
            keys(["n", "\r"])
            m.expect(SHELL_PROMPT, timeout=10)
        step("^X with changes asks; n leaves the file alone", asks)

        def keyboard():
            m.type_serial("/n/ata0p1/edit /n/ata0p1/KBD.TXT\r")
            m.expect("edit  /n/ata0p1/KBD.TXT", timeout=20)
            time.sleep(0.8)
            for k in ("end", "z", "left", "left", "y", "ctrl-o"):
                m.monitor.sendall(f"sendkey {k} 10\n".encode())
                time.sleep(0.3)
            m.expect("Saved", timeout=10)
            m.monitor.sendall(b"sendkey ctrl-x 10\n")
            m.expect(SHELL_PROMPT, timeout=10)
        step("the PS/2 keyboard: End, Left, Ctrl+O, Ctrl+X in raw mode", keyboard)

        def cooked_again():
            run_command(m, "serial", "echo cooked again", ["cooked again"], SHELL_PROMPT)
            run_command(m, "serial", "cat /dev/consctl", ["rawoff"], SHELL_PROMPT)
        step("afterwards the console is cooked again", cooked_again)

        def in_manide():
            # ManiDE's terminal: edit turns on term's raw mode (ESC [ ? 7727 h);
            # keys from the keyboard reach it as a terminal would send them.
            m.type_serial("manide -s /n/ata0p1/SESSION\r")
            m.expect("started term /n/ata0p1/edit /n/ata0p1/TERM.TXT", timeout=30)
            time.sleep(4)
            for k in ("end", "left", "left", "x", "down", "y", "ctrl-o"):
                m.monitor.sendall(f"sendkey {k} 10\n".encode())
                time.sleep(0.4)
            time.sleep(1)
            m.monitor.sendall(b"sendkey ctrl-x 10\n")
            time.sleep(1)
            for k in ("e", "x", "i", "t", "ret"):  # the shell in the pane: its line mode again
                m.monitor.sendall(f"sendkey {k} 10\n".encode())
                time.sleep(0.2)
            m.expect("exited", timeout=20)
        step("in ManiDE's terminal: keys in raw mode, then lines again", in_manide)
    finally:
        m.close()

    def files():
        def get(name):
            return subprocess.run(["mtype", "-i", f"{disk}@@{2048 * 512}", "::/" + name],
                                  capture_output=True).stdout
        want = {
            "NEW.TXT": b"Xhello world\nsecond line\n",
            "DOS.TXT": b"one\r\ntwo!three\r\n",
            "ABC.TXT": b"b\na\nc\n",
            "FIND.TXT": b"alpha\nbeta\ngamma *target\n#delta\n",
            "KEEP.TXT": b"keep me\n",
            "KBD.TXT": b"abycz\n",
            "TERM.TXT": b"in a paxyne\n",  # one line: Down stays on it
        }
        for name, data in want.items():
            got = get(name)
            need(got == data, f"{name}: {got!r}, wanted {data!r}")
    step("every file holds what was typed", files)
    print(f"{'OK' if not failures else 'FAILED'}: {failures} failure(s)")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
