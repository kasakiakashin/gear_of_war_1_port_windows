#!/usr/bin/env python3
"""
validate_phase1.py - integrity checks for the phase-1 outputs:
  * ISO extraction vs. the XDVDFS manifest (sizes + byte-exact spot checks)
  * Unreal Engine 3 package / Bink / Coalesced signatures
  * base_image.bin structural checks (PE, .pdata, .reloc, import thunks,
    PowerPC opcode sanity) that would fail on a corrupt LZX/AES decode.
Exits non-zero if any check fails.
"""
import argparse
import collections
import hashlib
import json
import os
import random
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from xdvdfs import XdvdfsImage, SECTOR  # noqa: E402

UE3_TAG = 0x9E2A83C1
results = []


def check(name, ok, detail=""):
    results.append((name, ok, detail))
    print(f"  [{'PASS' if ok else 'FAIL'}] {name}" + (f" - {detail}" if detail else ""))
    return ok


def validate_iso(iso, out_dir, manifest_path):
    print("== ISO extraction")
    man = json.load(open(manifest_path))
    missing, wrong = [], []
    for f in man["files"]:
        p = os.path.join(out_dir, *f["path"].split("/"))
        if not os.path.isfile(p):
            missing.append(f["path"])
        elif os.path.getsize(p) != f["size"]:
            wrong.append(f["path"])
    check("all manifest files present", not missing, f"{len(man['files'])} files, {len(missing)} missing")
    check("all file sizes match", not wrong, f"{len(wrong)} mismatched")
    total = sum(os.path.getsize(os.path.join(out_dir, *f["path"].split("/"))) for f in man["files"]
                if f["path"] not in missing)
    check("total bytes match", total == man["total_bytes"], f"{total:,} bytes")

    img = XdvdfsImage(iso)
    rnd = random.Random(1234)
    sample = rnd.sample(man["files"], min(40, len(man["files"])))
    sample += [f for f in man["files"] if f["path"] == "default.xex"]
    bad = 0
    for f in sample:
        p = os.path.join(out_dir, *f["path"].split("/"))
        n = min(f["size"], 1 << 20)
        off = rnd.randrange(0, max(f["size"] - n, 0) + 1)
        img.f.seek(img.base + f["sector"] * SECTOR + off)
        a = img.f.read(n)
        with open(p, "rb") as h:
            h.seek(off)
            b = h.read(n)
        if a != b:
            bad += 1
    img.close()
    check("byte-exact spot check vs ISO", bad == 0, f"{len(sample)} files sampled, {bad} differ")
    return man


def validate_ue3(out_dir, man):
    print("== Unreal Engine 3 content")
    by_ext = collections.Counter(os.path.splitext(f["path"])[1].lower() for f in man["files"])
    print("     extensions: " + ", ".join(f"{e or '<none>'}={c}" for e, c in by_ext.most_common()))
    pkgs = [f for f in man["files"] if f["path"].lower().endswith(".xxx")]
    versions = collections.Counter()
    bad, compressed = [], []
    for f in pkgs:
        path = os.path.join(out_dir, *f["path"].split("/"))
        with open(path, "rb") as h:
            hdr = h.read(16)
        tag, lic, ver = struct.unpack_from(">IHH", hdr)
        if tag == UE3_TAG:
            versions[(ver, lic)] += 1
            continue
        # fully compressed package: LE chunk header {tag, tag-as-blocksize, csize, usize}
        ltag, lblock, csize, usize = struct.unpack_from("<IIII", hdr)
        if ltag == UE3_TAG and lblock == UE3_TAG and 16 < csize <= f["size"]:
            compressed.append((f["path"], csize, usize))
        else:
            bad.append(f["path"])
    check("UE3 packages (*.xxx): BE tag 0x9E2A83C1 or compressed-chunk header", not bad and pkgs,
          f"{len(pkgs)} packages: {sum(versions.values())} plain "
          f"(file version/licensee {', '.join(f'{v}/{l}' for v, l in versions)}), "
          f"{len(compressed)} fully compressed ({', '.join(os.path.basename(p) for p, _, _ in compressed)}), "
          f"{len(bad)} bad")
    biks = [f for f in man["files"] if f["path"].lower().endswith(".bik")]
    badb = [f for f in biks if open(os.path.join(out_dir, *f["path"].split("/")), "rb").read(3) != b"BIK"]
    check("Bink movies start with 'BIK'", not badb, f"{len(biks)} movies")
    coal = [f for f in man["files"] if "/Coalesced." in f["path"]]
    check("Coalesced localisation files present", len(coal) > 0, ", ".join(sorted(os.path.basename(f["path"]) for f in coal)))
    toc = os.path.join(out_dir, "WarGame", "Xbox360TOC.txt")
    if os.path.exists(toc):
        lines = [l for l in open(toc, encoding="latin-1").read().splitlines() if l.strip()]
        check("Xbox360TOC.txt parsed", len(lines) > 0, f"{len(lines)} entries")
    return {"packages": len(pkgs), "package_versions": {f"{v}/{l}": c for (v, l), c in versions.items()}}


def validate_image(map_path):
    print("== base_image.bin")
    j = json.load(open(map_path))
    img = open(j["base_image"]["path"], "rb").read()
    base = int(j["load_address"], 16)
    size = len(img)
    check("base image size == XEX image size", size == int(j["image_size"], 16), f"0x{size:X}")
    check("sha1 matches binary_map.json", hashlib.sha1(img).hexdigest() == j["base_image"]["sha1"])
    check("PE machine is PowerPC BE (0x1F2)", j["pe"]["machine"].startswith("0x01F2"))
    check("PE entry == XEX entry", j["pe"]["entry_point"] == j["entry_point"], j["entry_point"])

    secs = j["pe"]["sections"]
    code = [(int(s["virtual_address"], 16), int(s["end"], 16)) for s in secs if "CNT_CODE" in s["flags"]]

    def in_code(a):
        return any(lo <= a < hi for lo, hi in code)

    funcs = json.load(open(j["pdata"]["full_table"]))
    addrs = [int(f["address"], 16) for f in funcs]
    check(".pdata entries sorted & 4-aligned", addrs == sorted(addrs) and all(a % 4 == 0 for a in addrs),
          f"{len(addrs)} functions")
    check(".pdata entries inside code sections", all(in_code(a) for a in addrs))
    overlaps = sum(1 for a, f, n in zip(addrs, funcs, addrs[1:]) if a + f["length"] > n)
    check(".pdata functions do not overlap", overlaps == 0, f"{overlaps} overlaps")
    check("entry point is a .pdata function start", int(j["entry_point"], 16) in set(addrs))

    # first-instruction statistics: real code begins with mflr r12 / stwu / etc.
    first = collections.Counter()
    for a in addrs:
        first[struct.unpack_from(">I", img, a - base)[0]] += 1
    mflr = first.get(0x7D8802A6, 0)
    check("many functions begin with 'mflr r12' (0x7D8802A6)", mflr > len(addrs) * 0.3,
          f"{mflr}/{len(addrs)} ({mflr * 100 / len(addrs):.1f}%)")

    # opcode sanity over .text: primary opcode 0 / undefined ones should be rare
    t = next(s for s in secs if s["name"] == ".text")
    lo = int(t["rva"], 16)
    hi = lo + int(t["virtual_size"], 16)
    words = struct.unpack_from(f">{(hi - lo) // 4}I", img, lo)
    invalid_primary = {1, 5, 6, 56, 57, 60, 61}
    nonzero = [w for w in words if w]
    bad = sum(1 for w in nonzero if (w >> 26) in invalid_primary)
    blr = sum(1 for w in words if w == 0x4E800020)
    check(".text decodes as PowerPC (invalid primary opcodes < 0.5%)", bad < len(nonzero) * 0.005,
          f"{len(words)} words, {bad} with invalid primary opcode, {blr} blr")

    # byte-exact proof: recompute the XEX page-descriptor SHA-1 chain independently
    from xex2 import Xex2
    x = Xex2(open(os.path.join(os.path.dirname(j["base_image"]["path"]), j["source"]), "rb").read())
    ok, n = x.verify_pages(img)
    check("XEX page SHA-1 chain matches decoded image", ok, f"{n} descriptors x 64 KiB pages")

    # XEX resources live past the PE sections; the title resource must be an XDBF
    res = j["optional_headers"].get("RESOURCE_INFO", {}).get("resources", [])
    xdbf = [r for r in res if img[int(r["address"], 16) - base:][:4] == b"XDBF"]
    check("title resource is an XDBF database", bool(xdbf),
          ", ".join(f"{r['name']}@{r['address']}" for r in xdbf))

    # import thunks / variables carry (type<<24 | ordinal) before load
    nb = 0
    nt = 0
    for lib in j["imports"]:
        for e in lib["imports"]:
            for key, typ in (("variable_address", 0), ("thunk_address", 1)):
                if e[key]:
                    nt += 1
                    v = struct.unpack_from(">I", img, int(e[key], 16) - base)[0]
                    if v != (typ << 24) | (lib["library_index"] << 16) | e["ordinal"]:
                        nb += 1
    check("import records decode to (type<<24|lib<<16|ordinal)", nb == 0, f"{nt} records")
    total = sum(l["function_count"] + l["variable_count"] for l in j["imports"])
    check("import library summary", total > 0,
          "; ".join(f"{l['name']}: {l['function_count']}f/{l['variable_count']}v" for l in j["imports"]))

    # recognisable strings
    hits = {s: img.find(s.encode()) for s in ("Unreal", "WarGame", "Engine.", "GearGame")}
    check("UE3 strings present in image", any(v >= 0 for v in hits.values()),
          ", ".join(f"{k}@0x{base + v:08X}" for k, v in hits.items() if v >= 0))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("iso")
    ap.add_argument("--out", default="extracted")
    ap.add_argument("--manifest", default="analysis/iso_manifest.json")
    ap.add_argument("--map", default="analysis/binary_map.json")
    args = ap.parse_args()
    man = validate_iso(args.iso, args.out, args.manifest)
    validate_ue3(args.out, man)
    validate_image(args.map)
    fails = [r for r in results if not r[1]]
    print(f"\n{len(results) - len(fails)}/{len(results)} checks passed")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
