#!/usr/bin/env python3
"""Build the ManiOS package repository (served at http://manios.nl/repo).

Usage: tools/mkrepo.py --downloads DIR --key KEYFILE --out DIR [--recipes repo/packages.json]

For each recipe in repo/packages.json this takes the named files from an
unmodified upstream package zip (found in --downloads), checks every path
is 8.3 (ManiOS writes FAT 8.3 names), and writes a reproducible ustar
archive OUT/pkgs/NAME-VERSION.tar. The upstream zip is copied to
OUT/src/ as the corresponding source (or the separate source archive a
recipe names in "source"; a self-extracting .exe zip works as "zip"). Version, licence and description
come from the package's APPINFO/*.LSM; the ManiOS status line comes from
docs/compatibility/database.json, and only Working programs are accepted.

The index OUT/index.txt is the format pkg (userland/bin/pkg.c) reads,
signed with Ed25519 into OUT/index.sig (hex). OUT/index.json carries the
same data for the website. KEYFILE holds the 32-byte seed in hex; it is
never stored in the repository.
"""
import argparse
import hashlib
import io
import json
import re
import shutil
import sys
import tarfile
import unicodedata
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
NAME_RE = re.compile(r"^[a-z0-9][a-z0-9_-]{0,7}$")
COMPONENT_RE = re.compile(r"^[A-Za-z0-9_~$!#%&-]{1,8}(\.[A-Za-z0-9_~$!#%&-]{1,3})?$")
MTIME = 1735689600  # 2025-01-01: archives do not change when rebuilt


def lsm(z):
    """Fields of the package's APPINFO/*.LSM, if it has one."""
    for n in z.namelist():
        if re.match(r"(?i)^appinfo/[^/]+\.lsm$", n):
            fields = {}
            for line in z.read(n).decode("latin-1").splitlines():
                m = re.match(r"^([A-Za-z-]+):\s*(.*)$", line)
                if m:
                    fields[m.group(1).lower()] = m.group(2).strip()
            return fields
    return {}


def one_line(s, limit):
    """One line of plain ASCII (the index is read by ManiOS's C code)."""
    s = " ".join(str(s).split())
    for a, b in (("\u2018", "'"), ("\u2019", "'"), ("\u201c", '"'), ("\u201d", '"'),
                 ("\u2013", "-"), ("\u2014", "-"), ("\u2026", "..."), ("\u00d7", "x"),
                 ("\u2192", "->"), ("\u2265", ">=")):
        s = s.replace(a, b)
    s = unicodedata.normalize("NFKD", s).encode("ascii", "ignore").decode()
    return s if len(s) <= limit else s[: limit - 3].rstrip() + "..."


def tar_of(files):
    """A reproducible ustar archive of {path: bytes}."""
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w", format=tarfile.USTAR_FORMAT) as t:
        for rel in sorted(files, key=str.upper):
            ti = tarfile.TarInfo(rel)
            ti.size = len(files[rel])
            ti.mtime = MTIME
            ti.mode = 0o644
            ti.uname = ti.gname = "manios"
            t.addfile(ti, io.BytesIO(files[rel]))
    return buf.getvalue()


def build_native(recipe, out, version):
    """A native package from this tree's build (ManiOS components such as
    ManiDOS): "files" maps a path in the package (bin/NAME or
    share/PKG/...) to a file under the repository root. Its commands are
    its bin/ files; pkg treats it as part of the system when they all are
    in /boot/bin already."""
    name = recipe["name"]
    if not NAME_RE.match(name):
        sys.exit(f"{name}: package names are 1-8 of a-z 0-9 _ -")
    files = {}
    for dst, src in recipe["files"].items():
        top = dst.split("/")[0]
        if top not in ("bin", "share") or (top == "share" and not dst.startswith(f"share/{name}/")):
            sys.exit(f"{name}: {dst}: native packages hold bin/ and share/{name}/ only")
        for c in dst.split("/"):
            if not COMPONENT_RE.match(c):
                sys.exit(f"{name}: {dst}: not an 8.3 name")
        path = ROOT / src
        if not path.is_file():
            sys.exit(f"{name}: {src} is missing (build ManiOS first: make)")
        files[dst] = path.read_bytes()
    cmds = {d.split("/", 1)[1]: d for d in files if d.startswith("bin/")}
    data = tar_of(files)
    tar_name = f"pkgs/{name}-{version}.tar"
    (out / tar_name).write_bytes(data)
    return {
        "name": name,
        "version": version,
        "kind": "native",
        "file": tar_name,
        "size": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
        "license": one_line(recipe["license"], 60),
        "title": one_line(recipe["title"], 90),
        "summary": one_line(recipe["summary"], 190),
        "status": one_line(recipe.get("status", "Part of ManiOS; tested by its test suite"), 190),
        "compat": None,
        "cmds": cmds,
        "env": [],
        "requires": recipe.get("requires", []),
        "source": f"https://github.com/manikineko/manios/tree/v{version}",
        "origin": f"https://github.com/manikineko/manios/tree/v{version}",
        "files": sorted(files, key=str.upper),
    }


def build(recipe, downloads, out, db, origin_base):
    name = recipe["name"]
    if not NAME_RE.match(name):
        sys.exit(f"{name}: package names are 1-8 of a-z 0-9 _ -")
    app = db.get(recipe["compat"])
    if not app or app["status"] != "Working":
        sys.exit(f"{name}: compatibility entry {recipe['compat']} is not Working")
    zpath = downloads / recipe["zip"]
    z = zipfile.ZipFile(zpath)
    info = lsm(z)
    version = recipe.get("version") or info.get("version")
    licence = recipe.get("license") or info.get("copying-policy")
    if not version or not licence:
        sys.exit(f"{name}: no version or licence (no LSM?)")
    version = version.replace(" ", "")
    files = {}
    skip = set(recipe.get("skip", []))
    for n in sorted(z.namelist()):
        if n.endswith("/") or n in skip:
            continue
        for src, dst in recipe["files"].items():
            if n == src or (src.endswith("/") and n.startswith(src)):
                rel = dst + n[len(src):] if src.endswith("/") else dst
                for c in rel.split("/"):
                    if not COMPONENT_RE.match(c):
                        sys.exit(f"{name}: {n} -> {rel}: not an 8.3 name")
                if rel.upper() in {k.upper() for k in files}:
                    sys.exit(f"{name}: {rel} twice")
                files[rel] = z.read(n)
                break
    for cmd, target in recipe["cmds"].items():
        if not NAME_RE.match(cmd):
            sys.exit(f"{name}: command {cmd}: 1-8 of a-z 0-9 _ -")
        if target.upper() not in {k.upper() for k in files}:
            sys.exit(f"{name}: command {cmd} runs {target}, which the package lacks")
    data = tar_of(files)
    stem = f"{name}-{re.sub(r'[^A-Za-z0-9._-]', '_', version)}"
    tar_name = f"pkgs/{stem}.tar"
    (out / tar_name).write_bytes(data)
    # The published source: the upstream package itself, or (for programs
    # whose package carries only binaries, such as Info-ZIP's self-extracting
    # DOS build) the separate upstream source archive the recipe names.
    spath = downloads / recipe.get("source", recipe["zip"])
    suffix = ".tar.gz" if spath.name.endswith(".tar.gz") else ".zip"
    src_name = f"src/{stem}{suffix}"
    shutil.copyfile(spath, out / src_name)
    origin = recipe["origin"]
    if not origin.startswith(("FreeDOS", "http://", "https://")):
        origin = origin_base + origin
    return {
        "name": name,
        "version": version,
        "kind": "dos",
        "file": tar_name,
        "size": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
        "license": one_line(licence, 60),
        "title": one_line(recipe.get("title") or info.get("title") or name, 90),
        "summary": one_line(recipe.get("summary") or info.get("description") or "", 190),
        "status": one_line(app["summary"], 190),
        "compat": recipe["compat"],
        "cmds": recipe["cmds"],
        "env": recipe.get("env", []),
        # Every DOS program runs through ManiDOS's dosrun.
        "requires": ["manidos"] + recipe.get("requires", []),
        "source": src_name,
        "origin": origin,
        "source_sha256": hashlib.sha256(spath.read_bytes()).hexdigest(),
        "source_origin": recipe.get("source_origin", origin),
        "files": sorted(files, key=str.upper),
    }


def index_text(pkgs, groups):
    lines = ["manios-repo 1"]
    for g, members in groups.items():
        lines.append(f"group {g} {' '.join(members)}")
    for p in pkgs:
        lines.append(f"pkg {p['name']}")
        for key in ("version", "kind", "file", "size", "sha256", "license", "title", "summary",
                    "status"):
            lines.append(f"{key} {p[key]}")
        for cmd, target in p["cmds"].items():
            lines.append(f"cmd {cmd}={target}")
        for e in p["env"]:
            lines.append(e)
        if p["requires"]:
            lines.append(f"requires {' '.join(p['requires'])}")
        lines.append(f"source {p['source']}")
        lines.append("end")
    return "\n".join(lines) + "\n"


def page(pkgs, public_key, version, groups):
    """The repository's web page (index.html), static: no script needed."""
    from html import escape as e
    rows = []
    for p in pkgs:
        cmds = ", ".join(f"<code>{e(c)}</code>" for c in p["cmds"])
        rows.append(
            f'<tr><td><span class="app-name">{e(p["name"])}</span>'
            f'<span class="app-version">{e(p["version"])}</span></td>'
            f'<td>{e(p["title"])}<br><small>{e(p["summary"])}</small></td>'
            f"<td>{cmds}</td>"
            f'<td><span class="pill green">{"ManiOS" if p["kind"] == "native" else "Working"}</span> '
            f'<small>{e(p["status"])}</small></td>'
            f'<td class="evidence">{e(p["license"])}</td>'
            f'<td class="evidence"><a href="{e(p["file"])}">package</a> '
            f'({p["size"] // 1024} KiB)<br><a href="{e(p["source"])}">source</a></td></tr>')
    return f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta name="description" content="The ManiOS package repository: software you install with pkg.">
<meta name="theme-color" content="#0c1016">
<title>Package repository — ManiOS</title>
<link rel="stylesheet" href="../assets/css/base.css">
<link rel="stylesheet" href="../assets/css/compatibility.css">
<style>.repo-howto pre{{overflow-x:auto;padding:1rem;border-radius:.5rem;background:rgba(127,127,127,.12)}}
.repo-howto code,.compat-results code{{font-family:ui-monospace,monospace}}</style>
</head>
<body class="compat-page">
<header>
  <a class="brand" href="../" aria-label="ManiOS home"><span class="mark" aria-hidden="true">M</span><strong>ManiOS <small>INDEPENDENT BY DESIGN</small></strong></a>
  <nav class="page-links" aria-label="Main navigation">
    <a href="../">Home</a>
    <a href="../compatibility/">Compatibility DB</a>
    <a href="./" aria-current="page">Packages</a>
    <a href="https://github.com/manikineko/manios" target="_blank" rel="noopener noreferrer">Source code ↗</a>
  </nav>
</header>
<main class="compat-main">
  <section class="compat-hero" aria-labelledby="repo-title">
    <div>
      <span class="eyebrow">MANIOS {e(version.upper())} AND LATER</span>
      <h1 id="repo-title">Package repository</h1>
      <p>Software you install on ManiOS with <code>pkg</code>. Every package here
      is a program that passed its task on ManiOS and whose licence allows
      redistribution. DOS programs run through <code>dosrun</code>; each comes
      with its original source package.</p>
    </div>
    <div class="compat-meta">
      <span>{len(pkgs)} packages</span>
      <a href="index.txt">index.txt</a> · <a href="index.sig">index.sig</a> · <a href="index.json">index.json</a>
    </div>
  </section>
  <section class="repo-howto compat-footnote" aria-labelledby="howto-title">
    <h2 id="howto-title">Install from ManiOS</h2>
    <p>ManiOS needs a network card and a writable FAT disk (packages go to
    <code>/n/DISK/manios/pkg</code>). Then:</p>
<pre>pkg update            # fetch the package list
pkg search archive    # look for something
pkg install arj zoo   # install; the commands work at once
pkg list -i           # what is installed
pkg remove zoo
pkg install @dos-tools   # a group of packages</pre>
    <p>Groups: {"; ".join(f"<code>@{e(g)}</code> ({e(' '.join(m))})" for g, m in groups.items()) or "none yet"}.
    DOS packages need <code>manidos</code>, ManiOS's DOS runtime; <code>pkg</code>
    installs it with them unless the system has it built in.</p>
    <p>ManiOS has no TLS yet, so <code>pkg</code> fetches this repository over
    plain HTTP and trusts none of it by default: the package list must carry a
    valid Ed25519 signature by the ManiOS repository key, and every package must
    match the size and SHA-256 that list gives it. Repository public key:
    <code>{public_key}</code>.</p>
  </section>
  <section class="compat-results" aria-labelledby="list-title">
    <div class="results-head"><div><span class="eyebrow">AVAILABLE NOW</span>
    <h2 id="list-title">Every package</h2></div></div>
    <div class="table-wrap"><table>
      <thead><tr><th scope="col">Package</th><th scope="col">What it is</th><th scope="col">Command</th>
      <th scope="col">On ManiOS</th><th scope="col">Licence</th><th scope="col">Download</th></tr></thead>
      <tbody>
{chr(10).join(rows)}
      </tbody>
    </table></div>
  </section>
</main>
<footer>
  <span>© 2026 The ManiOS Project · Manikineko × ZygHosting</span>
  <nav aria-label="Footer"><a href="../">Home</a><a href="../compatibility/">Compatibility DB</a><a href="../legal/">License &amp; credits</a></nav>
</footer>
</body>
</html>
"""


def write_cd(cd, out, pkgs):
    """The repository as the install CD carries it (ISO 9660, 8.3 names):
    INDEX.TXT and INDEX.SIG as signed, PKGS/NAME.TAR, and the sources as
    SRC/NAME.ZIP or SRC/NAME.TGZ. pkg reads a directory repository and
    looks for PKGS/NAME.TAR when the index's file name is not there."""
    shutil.rmtree(cd, ignore_errors=True)
    (cd / "PKGS").mkdir(parents=True)
    (cd / "SRC").mkdir()
    shutil.copyfile(out / "index.txt", cd / "INDEX.TXT")
    shutil.copyfile(out / "index.sig", cd / "INDEX.SIG")
    for p in pkgs:
        shutil.copyfile(out / p["file"], cd / "PKGS" / f"{p['name'].upper()}.TAR")
        src = out / p["source"]
        if src.is_file():
            ext = "TGZ" if src.name.endswith(".tar.gz") else "ZIP"
            shutil.copyfile(src, cd / "SRC" / f"{p['name'].upper()}.{ext}")
    (cd / "README.TXT").write_text(
        "The ManiOS package repository, as on http://manios.nl/repo, for\r\n"
        "installing without a network: pkg -u /n/CD/REPO update (CD: the\r\n"
        "drive's name in /n). SRC holds each DOS package's source; ManiOS's\r\n"
        "own source is at https://github.com/manikineko/manios.\r\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--recipes", default=ROOT / "repo/packages.json", type=Path)
    ap.add_argument("--downloads", required=True, type=Path)
    ap.add_argument("--key", required=True, type=Path)
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("--cd", type=Path, help="also write the install CD's copy (8.3 names) here")
    args = ap.parse_args()
    try:
        import nacl.signing
    except ImportError:
        sys.exit("mkrepo: needs PyNaCl (python3-nacl)")
    recipes = json.loads(args.recipes.read_text())
    db = {a["id"]: a for a in json.loads(
        (ROOT / "docs/compatibility/database.json").read_text())["apps"]}
    out = args.out
    for sub in ("pkgs", "src"):
        (out / sub).mkdir(parents=True, exist_ok=True)
    version = (ROOT / "VERSION").read_text().strip()
    pkgs = [build_native(r, out, version) if r.get("kind") == "native"
            else build(r, args.downloads, out, db, recipes["origin_base"])
            for r in sorted(recipes["packages"], key=lambda r: r["name"])]
    names = {p["name"] for p in pkgs}
    for p in pkgs:
        for q in p["requires"]:
            if q not in names:
                sys.exit(f"{p['name']} requires {q}, which is not in the repository")
    groups = recipes.get("groups", {})
    for g, members in groups.items():
        if not re.match(r"^[a-z0-9][a-z0-9_-]{0,15}$", g) or not members or any(m not in names for m in members):
            sys.exit(f"group {g}: needs a short name and packages that exist")
    text = index_text(pkgs, groups).encode()
    if any(b > 127 for b in text):
        sys.exit("mkrepo: the index must be ASCII")
    key = nacl.signing.SigningKey(bytes.fromhex(args.key.read_text().strip()))
    sig = key.sign(text).signature
    (out / "index.txt").write_bytes(text)
    (out / "index.sig").write_text(sig.hex() + "\n")
    (out / "index.json").write_text(json.dumps({
        "repository": 1,
        "manios_min": "0.29.0",
        "built_with": version,
        "public_key": bytes(key.verify_key).hex(),
        "groups": groups,
        "packages": pkgs,
    }, indent=1) + "\n")
    (out / "index.html").write_text(page(pkgs, bytes(key.verify_key).hex(), version, groups))
    # Every file the repository serves, for the website's SHA256SUMS.
    sums = []
    for f in sorted(p for p in out.rglob("*") if p.is_file() and p.name != "SHA256SUMS"):
        sums.append(f"{hashlib.sha256(f.read_bytes()).hexdigest()}  {f.relative_to(out)}")
    (out / "SHA256SUMS").write_text("\n".join(sums) + "\n")
    if args.cd:
        write_cd(args.cd, out, pkgs)
    print(f"mkrepo: {len(pkgs)} packages, index {len(text)} bytes, signed by "
          f"{bytes(key.verify_key).hex()[:16]}...")


if __name__ == "__main__":
    main()
