# 0.28.0 targeted retest — 2026-11-22

**Build:** ManiOS 0.28.0; kernel SHA-256 recorded in
[database.json](database.json) (`build.kernel_sha256`).
**Environment:** QEMU i386, `-cpu 486`, 64 MiB RAM. Each application ran on a
disposable copy of the FAT16 image used for its earlier test, with the
same recorded command. Functional checks then drove the program with
keystrokes (PS/2 keyboard or serial console, as noted) and inspected the
screen, screenshots or the files left on the image.

These are the entries whose recorded stopping point the 0.28.0 dosrun
changes address (VGA CRTC/DAC/attribute ports, CMOS clock, no-coprocessor
model, absent game/sound hardware, FS/GS and LSS/LFS/LGS, INT3, SMSW, LOCK,
default interrupt vectors, TSR exit, TRUENAME). See
[dosrun.md](../dosrun.md#hardware-and-cpu-model-0280) for those behaviours.

## Now Working

| Application | Before | Verified on 0.28.0 |
| --- | --- | --- |
| @Compare 1.6 | Unsupported (OUT 03D4h) | Side-by-side compare of A.TXT/B.TXT; "The files contain one mismatch." [Screenshot](2026-11-22-028-compare-16.png) |
| CQvert 95.7.1 | Unsupported (OUT 03D4h) | OUT.TXT holds `name=Zyg Test` / `value=one+two&three=4` decoded from the URL-encoded input. |
| CHOP 3.1 | Unsupported (OUT 03D4h) | `CHOP31.EXE IN.TXT -P3 -X -Z` writes IN.1/IN.2/IN.3 (10+10+9 bytes) rebuilding the 29-byte input. The `C:/IN.TXT` form writes no parts ("File not found"). |
| EVAL | Unsupported (x87 DB) | `2+3`=5, `7/2`=3.5, `2*3.25`=6.5, `10-4`=6, via its own floating-point emulation. |
| CALC 1.00 | Unsupported (OUT 00F0h) | After the shareware notice, `2+3` prints `5.00000`. The notice needs Return pressed twice (the first key is swallowed). |
| Bali's Calc | Unsupported (OUT 03D4h) | `12+30=` shows 42, `*2=` shows 84; Esc exits. [Screenshot](2026-11-22-028-balicalc-84.png) |
| CPUID 5.93 | Unsupported (x87 DB) | "This system has a i486 SX microprocessor" — the emulated 486-class CPU without a coprocessor. |
| SYSID 6.02 | Unsupported (OUT 03D4h) | 15-page report; PgDn/End/Esc work. [Page 1](2026-11-22-028-sysid-page1.png) |
| GW-BASIC 3.23 | Unsupported (IN 0201h) | `PRINT 2+3*4` → 14; a FOR/NEXT program RUN prints 1, 4, 9; SYSTEM exits. SAVE/LOAD untested. |

## Now Partial

| Application | Before | Observed on 0.28.0 |
| --- | --- | --- |
| As-Easy-As 5.7 | Unsupported (FPU/OUT 60h) | Sheets open; A1=12, A2=30, A3 `+A1+A2` = 42 ([screenshot](2026-11-22-028-aseasy-formula.png)). Serial-console input only; PS/2 keystrokes are ignored. Save untested. |
| DOSINFO 1.3 | Unsupported (OUT 03D4h) | Full report ([screenshot](2026-11-22-028-dosinfo-13.png)); drive sizes are dosrun's 8 MiB placeholders. |
| MEM 3.0 | Unsupported (OUT 03D4h) | Memory map and drive panel ([screenshot](2026-11-22-028-mem-30.png)); drive sizes are placeholders. |
| NUMCONV | Unsupported (OUT 03D4h) | Menu and choice work; digits typed at the number prompt are ignored (PS/2 and serial). |
| Hanoi | Unsupported (IN 0006h) | Automatic play solves 3 disks in 7 steps ([screenshot](2026-11-22-028-hanoi-solved.png)); user play untested. |
| TypeFast | Broken (blank) | Menus and level 1 run; words fall, misses counted ([screenshot](2026-11-22-028-typefast-play.png)); a hit was not verified. |
| PC-Calc | Unsupported (x87 DB) | Worksheet opens ([screenshot](2026-11-22-028-pc-calc.png)); cell entry cannot be committed with Enter. |
| Directory Maven | Unsupported (OUT 03D4h) | `/sC` scans C: and reports "1 drive(s) scanned"; the manager UI was not exercised. |
| CHAMP Galagon | Unsupported (returned at once) | Detects no sound card, then the title menu and hall of fame render in mode 13h with the VGA palette ([menu](2026-11-22-028-galagon-menu.png), [attract](2026-11-22-028-galagon-attract.png)). Play untested. |
| Dr Mind Lite | Unsupported (OUT 03C8h) | Code-breaking board renders in mode 13h ([screenshot](2026-11-22-028-drmind-board.png)). Play untested. |
| Galaxy Lite | Unsupported (opcode 00) | Evaluation notice screen renders ([screenshot](2026-11-22-028-galaxy-lite.png)); editing not exercised. |
| PC-File | Unsupported (F0) | Title screen renders ([screenshot](2026-11-22-028-pc-file.png)); database functions not exercised. |
| ANA 0.15 | Unsupported (x87 DB) | Starts and prints its banner; `--help` is treated as a file name. Analysis not exercised. |

## Still not working, with a new stopping point

| Application | Status | Observed on 0.28.0 |
| --- | --- | --- |
| CYLON4 | Unsupported (was Working) | A TSR. The 2026-10-25 run had in fact stopped on OUT 03D4h, so the Working entry was wrong. It now loads and terminates-and-stays-resident, which ends it: nothing remains resident after dosrun exits, so there is no animated cursor. |
| KyCalc 2.07 | Unsupported | Passes the x87 stop, then installs as a TSR (Ctrl+Alt+K); same TSR limit. |
| MenuMate 2.1 | Unsupported | "MS-DOS version 5.0 or higher is required" (dosrun reports 4.00). |
| SHOW 3.14 | Partial (unchanged) | `SHOW PC` passes the CMOS clock, then run-time error R6003 (divide by zero). |
| COPYEM 1.0 | Unsupported | Asks for the target and starts copying, then stops on INSB (6Ch). |
| FoxType, LHa, lzop, UnRAR | Unsupported | All carry an embedded CWSDPMI extender: they pass SMSW, obtain XMS memory, then stop reprogramming the interrupt controller (OUT 20h, ICW1) to enter protected mode. They need DPMI. |
| DOS/32A | Unsupported | `/?` prints its banner; a protected-mode extender cannot run here. |
| Mine Mayhem, Microsoft BASIC 5.28 | Unsupported / Broken | Now run into the interrupt vector table and stop at 0000:0016. |
| Ability Plus 3.0 | Unsupported | Passes FNINIT; stops on a group-7 instruction other than SMSW (0F 01 at 10EA:2399). |
| Drobos | Unsupported | Passes x87 and the attribute controller; stops on the VGA graphics controller (OUT 03CEh). |
| DOSStart | Unsupported | Passes IN 21h; still reports that it needs a VGA screen. |
| Spectra | Unsupported | Passes IN 21h; BASIC runtime "Illegal function call" at 1010:0063. |
| Ford Driving Simulator | Unsupported | Passes IN 21h; stops on the diskette controller (OUT 03F2h). |
| Nero 5 | Unsupported | No longer returns silently; stops on the VGA graphics controller (OUT 03CEh) while setting up its 640x480 display. |

Unchanged on this build: SPACE (placeholder free space, now listing only
mounted drives), STATUS (free memory over total), MD5SUM (no digest),
Mahjong (DPMI) and DOS Navigator (blank). WHATUHAV, COMPUTE,
tail, wcd and Uptime were not retested (binaries not available locally).

## Two fixes found by the regression run

The first full rerun on the 0.28.0 build failed SemWare TSE's Alt+X exit.
Pointing unset BIOS/DOS vectors at handlers made TSE find an INT 2Fh
handler and ask for the XMS entry point, which exposed two older dosrun
bugs, both fixed in 0.28.0:

- The XMS entry stub was written at `PSP:FE00`, which for a large EXE is
  inside the program, so asking for it corrupted TSE. It now lives at
  F000:0380 and enters the driver through a private trap that leaves AX
  unchanged (the old stub overwrote AL).
- INT 2Fh answered every multiplex number as if it were XMS (43h). Other
  multiplex calls now return unchanged ("not installed").

## Regression

All 26 dosrun host tests (including the new `dosrun_device_test.py`) and
the in-guest `dosrun_test.py` pass. The 82 entries still listed as Working
were re-verified with scripted workflows on this build:
[working retest](2026-11-22-028-working-retest.json).
