# ADR-0011: A Debian-style installer with profiles, configuration and packages

**Status:** Accepted (in progress)
**Date:** 2026-11-23
**Target:** ManiOS 0.30.0
**Extends:** [ADR-0006](0006-native-boot-loader-and-installer.md) (boot
area, hybrid ISO, `install`). Builds on `pkg` and the package repository
(0.29, [docs/pkg.md](../pkg.md)).

## Context

`install DISK [KEY=VALUE...]` (ADR-0006) copies the boot area the system
was started from to one 0xDA partition. Everything is in that one image:
kernel, shell, ManiDE, ManiOS's DOS runtime (`dos`, `dosrun`) and every
other program. The only configuration is the kernel command line.

People want ManiOS for different jobs: a desktop, a server, DOS programs,
games. A server has no use for ManiDE, but it might need an old DOS
program later. Since 0.29 `pkg` can install software, but an installed
system has nowhere to put it (packages need a writable FAT disk, and the
installer makes only the boot partition), and `pkg` has no dependencies.

What exists to build on:

- ZKT reads ISO 9660 CDs over ATAPI (`zkt/fs/iso9660.c`), so an
  installer can use files from the install CD.
- FAT12/16 read-write with 8.3 names (`zkt/fs/fat.c`). No FAT32, and no
  program that formats a disk.
- Boot configuration is the kernel command line (`sysname=`, `ip=`,
  `dns=`, `shell=`, `rc=`, `key=`, ...). `rc=PATH` runs a script first.
- At boot the kernel adds the first `/n/DISK/manios/pkg/bin` it finds
  after `/bin`, so installed commands work.
- Signed package lists and SHA-256-checked packages (`pkg`).
- No user accounts, one keyboard layout (US), English only.

## Decision

ManiOS gets an installer that works like Debian's: one ISO, a sequence of
plain text screens, each with a sensible default, and the same steps
available non-interactively. What gets installed is a **base system plus
packages**, chosen by **profile**.

### 1. One ISO, two boot areas

*Built in 0.30.0: `BASE.BIN` (`make`, `build/base/`); see the 0.30.0 notes.*

The ISO carries:

- `MANIOS.BIN`, the **full** boot area: what the live CD and the browser
  edition boot today, with everything in it.
- `BASE.BIN`, the **base** boot area: kernel, shell, core programs,
  networking, `pkg`, `install`. Without ManiDE, `dos` or `dosrun`.
- `REPO/`, an offline copy of the package repository in the same signed
  format: the ManiOS components and every published package. The
  installer then needs no network.

The live system stays complete. Installed systems start from the base and
add what their profile lists.

### 2. ManiOS components become packages

*Built in 0.30.0: `manidos`, `manide`, `requires`, groups, and a directory
(the CD's `REPO`) as a repository.*

New native packages, built from this repository by `tools/mkrepo.py`:

| Package   | What it holds                            |
|-----------|------------------------------------------|
| `manidos` | `dos`, `dosrun` (ManiOS's DOS runtime)   |
| `manide`  | ManiDE and its programs (`desktop`, `term`, `files`, ...) |

`pkg` gains **dependencies**: a `requires NAME...` line in the index.
`pkg install` installs what is required first, `pkg remove` refuses to
remove something another installed package requires (or `-f`), and
`mkrepo.py` adds `requires manidos` to every DOS package by itself. On a
server, `pkg install arj` then brings ManiDOS along.

### 3. Profiles (Debian's "tasks")

| Profile        | Packages                                       | Boots into |
|----------------|------------------------------------------------|------------|
| Desktop        | `manide`, `manidos`                            | ManiDE     |
| Server         | none                                           | shell      |
| Server + DOS   | `manidos` and a DOS tools set                  | shell      |
| Custom         | chosen one by one from the repository list     | chosen     |

Sets of packages are **groups** in the index (`group dos-tools arj zoo
unzip ...`), so `pkg install @dos-tools` works after installation too.
Groups to start with: `dos-tools`, later `dos-games` and `dev`.

### 4. The installer's steps

*Built in 0.30.0. One change from this plan: the kernel finds partitions
only when it starts, so `install` cannot install packages onto the new
data partition itself. It formats it with `SETUP.CFG` (and a copy of the
CD's signed packages), and `setup` installs them at the first start, then
restarts once, as Debian's base-config once did.*

As Debian's installer, every screen has a default and Enter moves on:

1. **Welcome**: install, or (on an installed system) upgrade.
2. **Keyboard**: US for now. The screen is there so layouts can come later.
3. **Network**: DHCP (default) or static `ip=`, gateway, DNS; tested by
   looking up `manios.nl`. Skipped when there is no network card.
4. **Host name** (`sysname=`).
5. **Disk**: pick a disk, then guided partitioning (default: boot
   partition, 8 MiB; the rest up to 2 GiB is a FAT16 data partition) or
   "use this existing FAT partition for data".
6. **Profile**: the table above.
7. **Packages**: from the CD by default, or from the network
   (`http://manios.nl/repo`), when newer ones are available.
8. **Summary**: everything that is about to happen; type `yes`.
9. **Install**: write the boot area, format the data partition, install
   the packages, write the configuration, then read everything back.
10. **Finish**: remove the CD and reboot.

ManiOS has no accounts or locales, so Debian's user and language steps
have no counterpart yet.

Non-interactive, for scripts and tests:

```
install -y --profile server --pkg arj,unzip --hostname srv1 \
        --net dhcp ata0
```

`install DISK KEY=VALUE...` keeps working as it does today.

### 5. Disk layout and configuration

```
MBR
partition 1  0xDA  boot area (BASE.BIN), 1 MiB onwards
partition 2  0x06  FAT16 "MANIOS", the data partition:
  MANIOS/PKG/        packages, as since 0.29
  MANIOS/ETC/RC      the start-up script (rc=)
  MANIOS/ETC/NET.CFG network settings, as KEY=VALUE lines
  HOME/              the user's files
```

Settings the kernel needs before any disk is read (`sysname=`, `ip=`,
`dns=`, `shell=`, `rc=`) stay on the boot area's command line, written by
the installer and editable at each boot as now. Everything else lives in
plain-text files under `MANIOS/ETC`, which the start-up script reads, so
it can be changed without reinstalling.

### 6. New pieces of code

- `mkfat`: a FAT16 formatter (C, userland), also usable on its own.
- `install`: the text screens, the profiles, a second partition.
- `pkg`: `requires`, groups, a `file:` repository (the CD's `REPO/`).
- `mkrepo.py`: native packages built from the tree, `requires`, groups.
- The build: `BASE.BIN` next to `MANIOS.BIN`, and `REPO/` on the ISO.
- `tests/install_test.py`: each profile installed in QEMU without a
  network, rebooted, and checked (Desktop starts ManiDE; Server has no
  `dosrun` until `pkg install arj`; packages survive a reboot).

## Alternatives considered

- **Separate ISOs per use (desktop, server).** Simple to build, but two
  downloads to explain and test, and a server could not add DOS later
  without the package path anyway.
- **Keep one boot area and leave ManiDOS/ManiDE in it.** `dos` and `dosrun`
  together are about 143 KB, so space is no reason. But then a
  "Server" install is the desktop under another name, and components
  cannot be updated without reinstalling.
- **Rebuild the boot area inside ManiOS for each profile.** It would need
  a boot-area writer in ManiOS; shipping two prebuilt boot areas does the
  same with what the build already has.
- **Network-only installation.** The installer would depend on manios.nl
  being up and reachable over plain HTTP. The CD's `REPO/` keeps it
  working offline, and the network is still there for newer packages.

## Consequences

- The installed disk is self-contained: packages no longer need a second
  disk (the 0.29 limitation goes away).
- Desktop and Server are real differences, and DOS can be added to a
  server at any time with `pkg install manidos` or any DOS package.
- ManiDE and ManiDOS can be updated with `pkg upgrade` instead of a
  reinstall. The base system still updates by rewriting the boot area
  (a later "upgrade" mode of the installer).
- The ISO grows by the offline repository (about 2 MiB today).
- The data partition is capped at 2 GiB until FAT32 support exists, and
  file names stay 8.3 until the FAT driver writes long names.
- `make test` grows: an install test per profile.

## References

- [ADR-0006](0006-native-boot-loader-and-installer.md): boot area and `install`
- [ADR-0008](0008-manidos.md), [ADR-0010](0010-running-dos-programs.md): ManiDOS
- [docs/pkg.md](../pkg.md): packages and the repository format
- Debian installer: <https://www.debian.org/releases/stable/installmanual>
