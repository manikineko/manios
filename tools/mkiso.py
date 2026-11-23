#!/usr/bin/env python3
"""Writes the ManiOS ISO image (M14, ADR-0006): ISO 9660 (ECMA-119),
bootable from a CD by El Torito "no emulation" (cdboot.bin), and -- the
same image written to a USB stick or used as a hard disk -- by the MBR
in its system area, whose partition entry points at the boot area. No
other tool is needed, and the same inputs give the same image.

    sector 0        the MBR boot code and a partition of type 0xDA
    sectors 16-18   primary volume, El Torito boot record, terminator
    then            path tables, the root directory, the boot catalog,
                    CDBOOT.BIN, MANIOS.BIN (the boot area), other files

Usage: tools/mkiso.py --bootarea AREA --cdboot CDBOOT --mbr MBR
                      [--volume ID] [--file NAME=PATH]... [--dir NAME=DIR]... -o OUT

--dir adds DIR's tree as the directory NAME (0.30.0: the installer's
offline package repository); every name in it must be 8.3. Directory
entries are sorted by name, as ISO 9660 asks (before 0.30.0 BOOT.CAT
came first).
The date is SOURCE_DATE_EPOCH's, or the ManiOS release's day.
"""
import argparse
import os
import struct
import sys
import time

SECTOR = 2048
PARTITION_TYPE = 0xDA


def both16(v):
    return struct.pack("<H", v) + struct.pack(">H", v)


def both32(v):
    return struct.pack("<I", v) + struct.pack(">I", v)


def sectors(n):
    return (n + SECTOR - 1) // SECTOR


def dir_name(name):
    ok = set("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_")
    if not name or len(name) > 8 or not set(name.upper()) <= ok:
        sys.exit(f"mkiso: directory {name!r} is not an 8-character name")
    return name.upper().encode()


def iso_name(name):
    base, _, ext = name.upper().partition(".")
    ok = set("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_")
    if not base or len(base) > 8 or len(ext) > 3 or not set(base + ext) <= ok:
        sys.exit(f"mkiso: {name!r} is not an 8.3 name")
    return f"{base}.{ext};1".encode()


class Iso:
    def __init__(self, volume, when):
        self.volume, self.t = volume, time.gmtime(when)

    def date17(self):
        t = self.t
        return (f"{t.tm_year:04}{t.tm_mon:02}{t.tm_mday:02}{t.tm_hour:02}{t.tm_min:02}"
                f"{t.tm_sec:02}00").encode() + b"\0"

    def date7(self):
        t = self.t
        return bytes([t.tm_year - 1900, t.tm_mon, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, 0])

    def record(self, name, lba, length, directory=False):
        rec = bytearray(33)
        rec[2:10] = both32(lba)
        rec[10:18] = both32(length)
        rec[18:25] = self.date7()
        rec[25] = 2 if directory else 0
        rec[28:32] = both16(1)
        rec[32] = len(name)
        rec += name
        if len(rec) % 2:
            rec += b"\0"
        rec[0] = len(rec)
        return bytes(rec)

    @staticmethod
    def pack(records):
        """Directory records laid out so that none crosses a sector."""
        out = bytearray()
        for rec in records:
            if len(out) % SECTOR + len(rec) > SECTOR:
                out += bytes(SECTOR - len(out) % SECTOR)
            out += rec
        return bytes(out)

    def build(self, files, boot_name, bootarea_name, mbr, subdirs=None):
        """files: [(8.3 name, bytes)] for the root, including the boot
        image and area; subdirs: {NAME: {"files": [...], "dirs": {...}}}."""
        path_l, path_m, root_lba = 19, 20, 21
        # Every directory, breadth first (path table order): its entry
        # name, its parent's number (1-based), the tree node.
        dirs = [(b"\0", 1, {"files": files, "dirs": subdirs or {}})]
        i = 0
        while i < len(dirs):
            for name in sorted(dirs[i][2]["dirs"], key=dir_name):
                dirs.append((dir_name(name), i + 1, dirs[i][2]["dirs"][name]))
            i += 1
        # Sizes first (records depend on names only), then places.
        def entries(k):
            node = dirs[k][2]
            names = [(dir_name(n), True, n) for n in node["dirs"]]
            names += [(iso_name(n), False, n) for n, _ in node["files"]]
            if k == 0:
                names.append((iso_name("BOOT.CAT"), False, "BOOT.CAT"))
            return sorted(names)
        size = []
        for k in range(len(dirs)):
            recs = [self.record(b"\0", 0, 0, True), self.record(b"\1", 0, 0, True)]
            recs += [self.record(n, 0, 0, d) for n, d, _ in entries(k)]
            size.append(max(1, sectors(len(self.pack(recs)))) * SECTOR)
        dir_lba, lba = [], root_lba
        for k in range(len(dirs)):
            dir_lba.append(lba)
            lba += size[k] // SECTOR
        catalog_lba = lba
        lba = catalog_lba + 1
        placed = []
        for name, data in files:
            placed.append((name, data, lba))
            lba += max(1, sectors(len(data)))
        file_at = [{} for _ in dirs]
        for k in range(1, len(dirs)):
            for name, data in dirs[k][2]["files"]:
                file_at[k][name] = (lba, len(data), data)
                lba += max(1, sectors(len(data)))
        file_at[0] = {name: (at, len(data), data) for name, data, at in placed}
        total = lba
        where = {name: (at, len(data)) for name, data, at in placed}
        child = {}  # (parent number, entry name) -> directory number
        for k in range(1, len(dirs)):
            child[(dirs[k][1], dirs[k][0])] = k

        def directory(k):
            parent = dirs[k][1] - 1
            recs = [self.record(b"\0", dir_lba[k], size[k], True),
                    self.record(b"\1", dir_lba[parent], size[parent], True)]
            for n, is_dir, orig in entries(k):
                if is_dir:
                    c = child[(k + 1, n)]
                    recs.append(self.record(n, dir_lba[c], size[c], True))
                elif orig == "BOOT.CAT" and k == 0:
                    recs.append(self.record(n, catalog_lba, SECTOR))
                else:
                    at, length, _ = file_at[k][orig]
                    recs.append(self.record(n, at, length))
            return self.pack(recs)

        image = bytearray(total * SECTOR)

        def put(sector, data):
            image[sector * SECTOR:sector * SECTOR + len(data)] = data

        # The primary volume descriptor.
        pvd = bytearray(SECTOR)
        pvd[0], pvd[1:6], pvd[6] = 1, b"CD001", 1
        pvd[8:40] = b"MANIOS".ljust(32)
        pvd[40:72] = self.volume.encode().ljust(32)
        pvd[80:88] = both32(total)
        pvd[120:124] = both16(1)
        pvd[124:128] = both16(1)
        pvd[128:132] = both16(SECTOR)
        pvd[132:140] = both32(10)
        pvd[140:144] = struct.pack("<I", path_l)
        pvd[148:152] = struct.pack(">I", path_m)
        pvd[156:190] = self.record(b"\0", root_lba, size[0], True)
        pvd[190:702] = b" " * 512  # volume set, publisher, preparer, application
        pvd[318:318 + 24] = b"THE MANIOS PROJECT".ljust(24)
        pvd[574:574 + 32] = b"MANIOS TOOLS/MKISO.PY".ljust(32)
        pvd[702:813] = b" " * 111  # copyright, abstract, bibliographic files
        pvd[813:830] = self.date17()
        pvd[830:847] = self.date17()
        pvd[847:864] = b"0" * 16 + b"\0"
        pvd[864:881] = self.date17()
        pvd[881] = 1
        put(16, pvd)

        # The El Torito boot record, and the terminator.
        brvd = bytearray(SECTOR)
        brvd[0], brvd[1:6], brvd[6] = 0, b"CD001", 1
        brvd[7:7 + 23] = b"EL TORITO SPECIFICATION"
        brvd[71:75] = struct.pack("<I", catalog_lba)
        put(17, brvd)
        put(18, bytes([255]) + b"CD001" + bytes([1]))

        # Path tables: every directory, breadth first.
        tl, tm = bytearray(), bytearray()
        for k, (name, parent, _) in enumerate(dirs):
            pad = b"\0" if len(name) % 2 else b""
            tl += bytes([len(name), 0]) + struct.pack("<I", dir_lba[k]) + struct.pack("<H", parent) + name + pad
            tm += bytes([len(name), 0]) + struct.pack(">I", dir_lba[k]) + struct.pack(">H", parent) + name + pad
        if len(tl) > SECTOR:
            sys.exit("mkiso: too many directories for one path table sector")
        pvd_table = len(tl)
        put(path_l, tl)
        put(path_m, tm)
        image[16 * SECTOR + 132:16 * SECTOR + 140] = both32(pvd_table)
        for k in range(len(dirs)):
            put(dir_lba[k], directory(k))
        for k in range(1, len(dirs)):
            for at, length, data in file_at[k].values():
                put(at, data)

        # The boot catalog: a validation entry and the default entry,
        # "no emulation", 4 virtual sectors (the 2048 bytes of cdboot).
        validation = bytearray(32)
        validation[0] = 1
        validation[4:4 + 6] = b"MANIOS"
        validation[30:32] = b"\x55\xAA"
        validation[28:30] = struct.pack("<H", -sum(struct.unpack("<16H", validation)) & 0xFFFF)
        boot_lba, boot_len = where[boot_name]
        entry = bytearray(32)
        entry[0] = 0x88
        entry[6:8] = struct.pack("<H", 4)
        entry[8:12] = struct.pack("<I", boot_lba)
        put(catalog_lba, bytes(validation + entry))

        # The files, the boot image told where the boot area is.
        area_lba, area_len = where[bootarea_name]
        for name, data, at in placed:
            if name == boot_name:
                data = bytearray(data)
                data[8:16] = struct.pack("<II", area_lba, area_len)
            put(at, data)

        # The system area's first sector: the MBR, for booting the image
        # as a disk, with a partition over the boot area.
        mbr_sector = bytearray(512)
        mbr_sector[:446] = mbr
        start, count = area_lba * (SECTOR // 512), (area_len + 511) // 512
        mbr_sector[446:462] = struct.pack("<B3sB3sII", 0x80, b"\xFE\xFF\xFF", PARTITION_TYPE,
                                          b"\xFE\xFF\xFF", start, count)
        mbr_sector[510:512] = b"\x55\xAA"
        image[0:512] = mbr_sector
        return bytes(image)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bootarea", required=True)
    ap.add_argument("--cdboot", required=True)
    ap.add_argument("--mbr", required=True)
    ap.add_argument("--volume", default="MANIOS")
    ap.add_argument("--file", action="append", default=[], help="NAME=PATH")
    ap.add_argument("--dir", action="append", default=[], help="NAME=DIR")
    ap.add_argument("-o", dest="out", required=True)
    a = ap.parse_args()
    read = lambda path: open(path, "rb").read()
    cdboot, mbr = read(a.cdboot), read(a.mbr)
    if len(cdboot) != SECTOR or len(mbr) != 446:
        sys.exit("mkiso: cdboot must be 2048 bytes and the MBR code 446")
    files = [("CDBOOT.BIN", cdboot), ("MANIOS.BIN", read(a.bootarea))]
    for spec in a.file:
        name, _, path = spec.partition("=")
        files.append((name, read(path)))
    def tree(path):
        node = {"files": [], "dirs": {}}
        for entry in sorted(os.listdir(path)):
            full = os.path.join(path, entry)
            if os.path.isdir(full):
                node["dirs"][entry] = tree(full)
            else:
                node["files"].append((entry, read(full)))
        return node
    subdirs = {}
    for spec in a.dir:
        name, _, path = spec.partition("=")
        subdirs[name] = tree(path)
    when = int(os.environ.get("SOURCE_DATE_EPOCH", "1790294400"))  # 2026-09-25
    image = Iso(a.volume, when).build(files, "CDBOOT.BIN", "MANIOS.BIN", mbr, subdirs)
    with open(a.out, "wb") as f:
        f.write(image)


if __name__ == "__main__":
    main()
