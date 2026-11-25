# Packages: `pkg` and the repository (0.29.0 and later)

ManiOS 0.29.0 installs software from the ManiOS package repository at
<http://manios.nl/repo/>. The repository holds DOS programs (run through
[`dosrun`](dosrun.md)) and, as they arrive, native ManiOS programs. Only
programs that passed their task on ManiOS ([compatibility list](dos-compatibility.md))
and whose licence allows redistribution are published.

## Using it

ManiOS needs a network card (QEMU: `-nic user,model=e1000`; the default
DHCP setup also learns the DNS server) and a writable FAT disk, where
packages go: the first `/n/DISK` that can hold `manios/pkg`, or
`pkg -r DIR`.

```
pkg update              fetch and check the package list
pkg list                everything in the repository
pkg list -i             what is installed
pkg search WORD         by name, title or description
pkg info NAME           details: licence, size, command, ManiOS test result
pkg install NAME...     download, check and install (and what they need)
pkg install @GROUP      a group of packages, such as @dos-tools
pkg remove NAME...      uninstall (not what an installed package needs)
pkg remove -f NAME...   uninstall even so
pkg upgrade             install newer versions of what is installed
pkg -u URL update       use another repository (remembered for later commands)
pkg -u /n/CD/REPO update  a directory as the repository, such as the install CD (0.30.0)
```

An installed program's command works at once, and after a reboot too: the
kernel adds the first `/n/DISK/manios/pkg/bin` it finds after `/bin`. A
DOS package's command that would hide a built-in program gets a `d`
prefix (`dgrep`, `dhead`, `dtee`, `dsleep`).

### Dependencies and groups (0.30.0)

A package can need others (`requires` in the index); `pkg install`
installs them first and says so (`pkg: arj needs manidos`), and
`pkg remove` will not take away a package another installed package
needs. Every DOS package needs **`manidos`**, ManiOS's DOS runtime (`dos`
and `dosrun`). A native package whose commands are all in the boot archive
(`/boot/bin`) is part of the system already: on a system with ManiDOS
built in, `pkg` never downloads `manidos` (`pkg info manidos` says
*Part of this system*). Systems installed without ManiDOS (the server
profile of the 0.30 installer, [ADR-0011](adr/0011-debian-style-installer.md))
get it with their first DOS package.

Groups are sets of packages: `pkg list` shows them, and `pkg install
@dos-tools` installs the DOS tools in one go.

`hget URL [FILE]` fetches any `http://` URL with the same HTTP client.

DOS programs draw an 80x25 text screen. In ManiDE, give the terminal the
whole screen first (Alt+Space switches to the full-screen layout), or
their output does not fit the small tile.

## Trust

ManiOS has no TLS, so `pkg` uses plain HTTP and believes nothing because
of where it came from:

- `index.txt` must carry a valid Ed25519 signature (`index.sig`, hex) by
  the repository key built into `pkg`:
  `dd9cb68b51b10deb64aa80e455a711ad88ba6a17dd84fc561c3b9091fcf06020`.
  The saved index is checked again every time it is used.
- Each package must have exactly the size and SHA-256 the signed index
  gives it, or it is not installed.
- Archive entries must be relative 8.3 paths without `..`; native packages
  may only write `bin/` and `share/NAME/`, DOS packages only their own
  `dos/NAME/` directory.

A directory repository (the install CD's `REPO`) is checked the same
way: its `INDEX.TXT` needs the signature, each package its SHA-256.

An attacker on the network can stop an install, or serve an older signed
index, but cannot make `pkg` install bytes the key did not sign.

## On the disk

```
/n/DISK/manios/pkg/
  bin/            commands: native programs, and DOS launchers
  dos/NAME/       a DOS package's files
  share/NAME/     a native package's data
  db/NAME.lst     "NAME VERSION", then every file installed
  index.txt/.sig  the last checked package list; repo.url, its address
  cache/          downloads in progress
```

Names are 8.3, as ManiOS writes FAT: package and command names are at
most 8 characters.

A DOS launcher is a script the kernel runs through its interpreter
(`#!`, new in 0.29.0):

```
#!/bin/dosrun --launcher
# pkg: tee 2.0.3a
program /n/ata0p1/manios/pkg/dos/tee/TEE.EXE
env LANG=EN
envpath NLSPATH=/n/ata0p1/manios/pkg/dos/tee/NLS
```

`envpath` values become DOS paths; a launched program starts in the
shell's current directory as its DOS drive and directory.

## The repository format

`index.txt` is ASCII, version line `manios-repo 1`, then one block per
package:

```
pkg NAME
version 2.78a
kind dos | native
file pkgs/NAME-VERSION.tar
size BYTES
sha256 HEX
license ...
title ...
summary ...
status  the ManiOS test result
cmd COMMAND=FILE        (DOS: a launcher for FILE in the package;
                         native: bin/COMMAND, its own command)
requires PKG...         packages this one needs (0.30.0)
minos VERSION           the oldest ManiOS it runs on (0.30.1)
env NAME=VALUE          (DOS launchers)
envpath NAME=DIR        (DOS launchers; DIR inside the package)
source src/NAME-VERSION.zip   (or .tar.gz)
end
```

Between packages, `group NAME PKG...` lines name groups (0.30.0).
Unknown keys and lines are ignored, so later versions can add fields:
0.29 `pkg` reads a 0.30 index and simply installs no dependencies. Packages are
ustar archives. Each DOS package's unmodified upstream package (which
includes its source code) is published beside it under `src/`; where the
upstream package carries only binaries (Info-ZIP UnZip), its upstream
source archive is published instead.

## Building and publishing the repository

Recipes are in [`repo/packages.json`](../repo/packages.json): which files
of which upstream package, which commands, any environment, and the
groups. A recipe with `"kind": "native"` is built from this tree's build
output (`make` first): ManiOS components such as `manidos`, versioned as
`VERSION`. `mkrepo.py` adds `requires manidos` to every DOS package and
checks that every requirement and group member exists. Native programs of
their own (such as `edit`) live in `apps/NAME/` and build to
`build/apps/bin/NAME`, outside the system. Then

```sh
python3 tools/mkrepo.py --downloads DIR_WITH_UPSTREAM_ZIPS \
    --key PATH/TO/manios-repo-ed25519.key --out OUT
```

writes `OUT/index.txt`, `index.sig`, `index.json`, `index.html` (the web
page), `pkgs/`, `src/` and `SHA256SUMS`. It refuses a recipe whose
program is not Working in `docs/compatibility/database.json`, a name that
is not 8.3, or a command whose file is missing. The private key is never
in the repository; keep it with the release signing material.

Publish `OUT` as `/repo/` on the website. It must be reachable over plain
HTTP: ManiOS cannot follow a redirect to HTTPS. With Nginx, in the
`server` block that listens on port 80 (before any `return 301` to
HTTPS):

```nginx
location /repo/ {
    root /var/www/manios;
    try_files $uri $uri/ =404;
}
```

and reload Nginx (`sudo nginx -t && sudo systemctl reload nginx`).
Browsers can still use `https://manios.nl/repo/`.

manios.nl sits behind Nginx Proxy Manager, which does HTTPS. There, on the
manios.nl proxy host: **SSL** tab, *Force SSL* off (keep the certificate);
**Advanced** tab, this redirect that spares `/repo/`:

```nginx
set $force_https "";
if ($scheme = "http") { set $force_https "A"; }
if ($request_uri !~ "^/repo/") { set $force_https "${force_https}B"; }
if ($force_https = "AB") { return 301 https://$host$request_uri; }
```

Everything else still redirects to HTTPS (set up 2026-11-23).
