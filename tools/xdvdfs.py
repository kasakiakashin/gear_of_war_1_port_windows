#!/usr/bin/env python3
"""
xdvdfs.py - Xbox / Xbox 360 XDVDFS (XGD) image reader and extractor.

Written from scratch for the Gears of War port project (phase 1).

Layout notes
------------
* An Xbox 360 retail DVD image (XGD2) is a hybrid disc: a small ISO9660/UDF
  "video" partition followed by the game partition. The game partition starts
  at a fixed offset that depends on the disc generation:
      XGD1 (original Xbox)   0x18300000
      XGD2 (Xbox 360)        0x0FD90000
      XGD3 (Xbox 360)        0x02080000
      plain XISO (rebuilt)   0x00000000
* Inside the partition, sector 32 (byte 0x10000) holds the volume descriptor:
      0x000  char[20]  "MICROSOFT*XBOX*MEDIA"
      0x014  u32 LE    root directory sector
      0x018  u32 LE    root directory size (bytes)
      0x01C  u64 LE    FILETIME of mastering
      0x7EC  char[20]  "MICROSOFT*XBOX*MEDIA"
* A directory is a binary search tree of entries stored in one or more
  2048-byte sectors (entries never straddle a sector; padding is 0xFF):
      0x00 u16 LE  left subtree offset  (in 4-byte units, 0 = none)
      0x02 u16 LE  right subtree offset (in 4-byte units, 0 = none)
      0x04 u32 LE  start sector (relative to the partition)
      0x08 u32 LE  size in bytes
      0x0C u8      attributes (0x10 = directory)
      0x0D u8      name length
      0x0E char[n] name
  All values are little-endian. Sector numbers are relative to the partition.
"""
import argparse
import json
import os
import struct
import sys
import time

SECTOR = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"
PARTITION_CANDIDATES = [
    ("XGD2", 0x0FD90000),
    ("XGD3", 0x02080000),
    ("XGD1", 0x18300000),
    ("XISO", 0x00000000),
]
ATTR_DIRECTORY = 0x10


class XdvdfsError(Exception):
    pass


class Entry:
    __slots__ = ("name", "sector", "size", "attrs", "path")

    def __init__(self, name, sector, size, attrs, path):
        self.name = name
        self.sector = sector
        self.size = size
        self.attrs = attrs
        self.path = path

    @property
    def is_dir(self):
        return bool(self.attrs & ATTR_DIRECTORY)


class XdvdfsImage:
    def __init__(self, path):
        self.path = path
        self.f = open(path, "rb")
        self.f.seek(0, os.SEEK_END)
        self.image_size = self.f.tell()
        self.kind, self.base = self._find_partition()
        vd = self._read(self.base + 32 * SECTOR, SECTOR)
        self.root_sector, self.root_size, self.filetime = struct.unpack_from("<IIQ", vd, 0x14)

    def close(self):
        self.f.close()

    def _read(self, off, n):
        self.f.seek(off)
        data = self.f.read(n)
        if len(data) != n:
            raise XdvdfsError(f"short read at 0x{off:X} ({len(data)}/{n})")
        return data

    def _find_partition(self):
        for kind, off in PARTITION_CANDIDATES:
            vd_off = off + 32 * SECTOR
            if vd_off + SECTOR > self.image_size:
                continue
            vd = self._read(vd_off, SECTOR)
            if vd[:20] == MAGIC and vd[0x7EC:0x800] == MAGIC:
                return kind, off
        raise XdvdfsError("XDVDFS volume descriptor not found at any known partition offset")

    def read_dir(self, sector, size, parent=""):
        """Walk one directory's binary tree; returns entries in tree (sorted) order."""
        if size == 0:
            return []
        data = self._read(self.base + sector * SECTOR, (size + SECTOR - 1) // SECTOR * SECTOR)
        out = []
        seen = set()
        stack = [0]
        # iterative in-order traversal is not required; collect then sort by name
        while stack:
            off = stack.pop()
            if off in seen or off + 14 > len(data):
                continue
            seen.add(off)
            left, right, start, fsize, attrs, nlen = struct.unpack_from("<HHIIBB", data, off)
            if left == 0xFFFF and right == 0xFFFF:  # padding / empty dir marker
                continue
            name = data[off + 14: off + 14 + nlen].decode("latin-1")
            path = f"{parent}/{name}" if parent else name
            out.append(Entry(name, start, fsize, attrs, path))
            if left:
                stack.append(left * 4)
            if right:
                stack.append(right * 4)
        out.sort(key=lambda e: e.name.upper())
        return out

    def walk(self):
        """Yield every entry (files and dirs), depth-first."""
        stack = [("", self.root_sector, self.root_size)]
        while stack:
            parent, sec, size = stack.pop()
            for e in self.read_dir(sec, size, parent):
                yield e
                if e.is_dir:
                    stack.append((e.path, e.sector, e.size))

    def extract_file(self, entry, dest, bufsize=8 * 1024 * 1024):
        self.f.seek(self.base + entry.sector * SECTOR)
        remaining = entry.size
        with open(dest, "wb") as o:
            while remaining:
                chunk = self.f.read(min(bufsize, remaining))
                if not chunk:
                    raise XdvdfsError(f"unexpected EOF extracting {entry.path}")
                o.write(chunk)
                remaining -= len(chunk)


def filetime_to_iso(ft):
    if not ft:
        return None
    secs = ft / 10_000_000 - 11644473600
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(secs))


def main():
    ap = argparse.ArgumentParser(description="Extract an Xbox 360 XDVDFS game partition")
    ap.add_argument("iso", help="path to the disc image (.iso)")
    ap.add_argument("-o", "--out", default="extracted", help="output directory")
    ap.add_argument("-l", "--list", action="store_true", help="only list files")
    ap.add_argument("--only", action="append", default=None,
                    help="extract only this path (repeatable), e.g. --only default.xex")
    ap.add_argument("--manifest", default=None, help="write a JSON manifest of the file tree")
    args = ap.parse_args()

    img = XdvdfsImage(args.iso)
    print(f"[xdvdfs] image      : {args.iso} ({img.image_size:,} bytes)")
    print(f"[xdvdfs] partition  : {img.kind} at 0x{img.base:X}")
    print(f"[xdvdfs] root dir   : sector {img.root_sector} size {img.root_size}")
    print(f"[xdvdfs] mastered   : {filetime_to_iso(img.filetime)}")

    entries = list(img.walk())
    files = [e for e in entries if not e.is_dir]
    dirs = [e for e in entries if e.is_dir]
    total = sum(e.size for e in files)
    print(f"[xdvdfs] {len(files)} files, {len(dirs)} directories, {total:,} bytes")

    if args.only:
        wanted = {p.replace("\\", "/").lower() for p in args.only}
        for e in files:
            if e.path.lower() in wanted:
                dest = os.path.join(args.out, *e.path.split("/"))
                os.makedirs(os.path.dirname(os.path.abspath(dest)), exist_ok=True)
                img.extract_file(e, dest)
                wanted.discard(e.path.lower())
                print(f"[xdvdfs] {e.path} -> {dest}")
        if wanted:
            raise XdvdfsError(f"not found in the image: {sorted(wanted)}")
    elif args.list:
        for e in entries:
            print(f"{'D' if e.is_dir else 'F'} {e.size:>12} {e.path}")
    else:
        os.makedirs(args.out, exist_ok=True)
        done = 0
        t0 = time.time()
        for d in dirs:
            os.makedirs(os.path.join(args.out, *d.path.split("/")), exist_ok=True)
        for i, e in enumerate(sorted(files, key=lambda e: e.sector)):  # sequential disc order
            dest = os.path.join(args.out, *e.path.split("/"))
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            if os.path.exists(dest) and os.path.getsize(dest) == e.size:
                done += e.size
                continue
            img.extract_file(e, dest)
            done += e.size
            if i % 200 == 0:
                rate = done / max(time.time() - t0, 1e-6) / 1e6
                print(f"  [{i + 1}/{len(files)}] {done * 100 / max(total, 1):5.1f}%  {rate:6.1f} MB/s  {e.path}")
        print(f"[xdvdfs] extracted {len(files)} files to {args.out} in {time.time() - t0:.1f}s")

    if args.manifest:
        os.makedirs(os.path.dirname(os.path.abspath(args.manifest)), exist_ok=True)
        with open(args.manifest, "w", encoding="utf-8") as m:
            json.dump({
                "image": os.path.basename(args.iso),
                "partition_type": img.kind,
                "partition_offset": f"0x{img.base:X}",
                "root_sector": img.root_sector,
                "mastered": filetime_to_iso(img.filetime),
                "file_count": len(files),
                "dir_count": len(dirs),
                "total_bytes": total,
                "files": [{"path": e.path, "sector": e.sector, "size": e.size} for e in sorted(files, key=lambda e: e.path)],
            }, m, indent=1)
        print(f"[xdvdfs] manifest written to {args.manifest}")
    img.close()


if __name__ == "__main__":
    sys.exit(main())
