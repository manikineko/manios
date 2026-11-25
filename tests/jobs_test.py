#!/usr/bin/env python3
"""Jobs, kill, ^C and making files from the shell (0.31.0):

  - `cmd &` runs in the background: the shell says its pid, `jobs`
    lists it, and before a later prompt says it is done;
  - `kill PID` stops a background job (even one asleep), `kill` of a pid
    that isn't there says so;
  - ^C stops the command the shell waits for, even one blocked in
    accept() or reading a pipe, and at a prompt drops the line typed;
  - a program stopped this way frees its port: a new listener gets it;
  - `> FILE` makes or empties a file, `>> FILE` adds to it.

Usage: tests/jobs_test.py [path-to-kernel-elf]
"""
import os
import re
import sys
import tempfile
import time

from console_test import SHELL_PROMPT, Machine, TestFailure, run_command
from dosrun_test import build_disk, mt, put


def main():
    kernel = sys.argv[1] if len(sys.argv) > 1 else "build/manios-zkt.elf"
    work = tempfile.mkdtemp(prefix="zkt-jobs-")
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
            print(f"FAIL: {name}: {e}\n  tail: {m.output[-600:]!r}")
            failures += 1

    def need(cond, msg):
        if not cond:
            raise TestFailure(msg)

    def sh(cmd, want=(), timeout=20):
        m.type_serial(cmd + "\r")
        out = m.expect(SHELL_PROMPT, timeout=timeout)
        for w in want:
            if w.startswith("!"):
                need(w[1:] not in out, f"{cmd!r}: unexpected {w[1:]!r} in {out!r}")
            else:
                need(w in out, f"{cmd!r}: no {w!r} in {out!r}")
        return out

    m = Machine(kernel, ["-drive", f"file={disk},format=raw,if=ide", "-nic", "user,model=e1000"],
                memory=48)
    try:
        m.expect(SHELL_PROMPT, timeout=90)

        def background():
            out = sh("sleep 2 &")
            pid = re.search(r"\[(\d+)\]", out)
            need(pid, f"no pid: {out!r}")
            sh("jobs", [f"[{pid.group(1)}] running  sleep"])
            time.sleep(3)
            sh("echo next", [f"[{pid.group(1)}] done (0)  sleep"])
            sh("jobs", ["!running"])
        step("cmd &: the pid, jobs, done before a later prompt", background)

        def kill_job():
            out = sh("sleep 600 &")
            pid = re.search(r"\[(\d+)\]", out).group(1)
            out = sh(f"kill {pid}", ["!no such"]) + sh("echo after")
            need(f"[{pid}] killed  sleep" in out, f"no word of the killed job: {out!r}")
            sh("kill 4000", ["kill: 4000: no such process"])
            sh("cat /dev/ps", ["!sleep"])
        step("kill: a sleeping background job; a pid that isn't there", kill_job)

        def ctrl_c():
            m.type_serial("/n/ata0p1/LTEST 8080 1\r")
            m.expect("listening on 8080", timeout=20)
            time.sleep(0.5)  # the shell names it as the foreground just after starting it
            m.type_serial("\x03")
            out = m.expect(SHELL_PROMPT, timeout=10)
            need("^C" in out, f"no ^C echoed: {out!r}")
            # The port is free again: a new listener gets it.
            m.type_serial("/n/ata0p1/LTEST 8080 1\r")
            m.expect("listening on 8080", timeout=20)
            time.sleep(0.5)  # the shell names it as the foreground just after starting it
            m.type_serial("\x03")
            m.expect(SHELL_PROMPT, timeout=10)
            m.type_serial("cat | cat\r")
            time.sleep(1)
            m.type_serial("\x03")
            m.expect(SHELL_PROMPT, timeout=10)
            m.type_serial("echo typed-but-dropped")
            time.sleep(0.5)
            m.type_serial("\x03")
            out = m.expect(SHELL_PROMPT, timeout=10)
            need("typed-but-dropped\r\n" not in out.replace("echo typed-but-dropped", ""),
                 f"the dropped line ran: {out!r}")
            sh("echo still-here", ["still-here"])
        step("^C: a program in accept(), a pipeline, a line at the prompt; the port freed", ctrl_c)

        def files():
            sh("echo first > /n/ata0p1/NEW.TXT", ["!sh:"])
            sh("echo second >> /n/ata0p1/NEW.TXT", ["!sh:"])
            sh("cat /n/ata0p1/NEW.TXT", ["first\r\nsecond"])
            sh("echo replaced > /n/ata0p1/NEW.TXT")
            out = sh("cat /n/ata0p1/NEW.TXT", ["replaced"])
            need("first" not in out.split("NEW.TXT", 1)[1], f"> did not empty the file: {out!r}")
            sh("echo nothing > /dev/null", ["!sh:"])
        step("> makes and empties files, >> adds; > /dev/null still works", files)
    finally:
        m.close()
    print(f"{'OK' if not failures else 'FAILED'}: {failures} failure(s)")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
