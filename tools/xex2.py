#!/usr/bin/env python3
"""
xex2.py - XEX2 (Xbox 360 executable) parser and base-image extractor.

Written from scratch for the Gears of War port project (phase 1).

Produces:
  * the decrypted + decompressed PE32 (PowerPC big-endian) base image
  * a JSON map with load address, entry point, PE sections, optional headers,
    import libraries / ordinals and the .pdata function table.

XEX2 layout (all integers big-endian)
-------------------------------------
  0x00 'XEX2'
  0x04 u32 module flags
  0x08 u32 PE data offset (size of all headers)
  0x0C u32 reserved
  0x10 u32 security info offset
  0x14 u32 optional header count
  0x18 { u32 key, u32 value } * count
       key & 0xFF == 0x00/0x01 -> value is the data itself
       key & 0xFF == 0xFF      -> value is file offset, data starts with u32 size
       otherwise               -> value is file offset, size = (key & 0xFF) * 4
Security info:
  0x000 u32 header size        0x004 u32 image size
  0x008 rsa[256]               0x108 u32 info size
  0x10C u32 image flags        0x110 u32 load address
  0x114 section digest[20]     0x128 u32 import table count
  0x12C import digest[20]      0x140 media id[16]
  0x150 aes key[16]            0x160 u32 export table
  0x164 header digest[20]      0x178 u32 game regions
  0x17C u32 allowed media      0x180 u32 page descriptor count
  0x184 { u32 (pages << 4 | info), digest[20] } * count
"""
import argparse
import hashlib
import json
import os
import struct
import sys
import time

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lzx  # noqa: E402

XEX2_RETAIL_KEY = bytes.fromhex("20B185A59D28FDC340583FBB0896BF91")
XEX2_DEVKIT_KEY = bytes(16)

OPT_HEADER_NAMES = {
    0x000002FF: "RESOURCE_INFO",
    0x000003FF: "FILE_FORMAT_INFO",
    0x00000405: "BASE_REFERENCE",
    0x000005FF: "DELTA_PATCH_DESCRIPTOR",
    0x000080FF: "BOUNDING_PATH",
    0x00008105: "DEVICE_ID",
    0x00010001: "ORIGINAL_BASE_ADDRESS",
    0x00010100: "ENTRY_POINT",
    0x00010201: "IMAGE_BASE_ADDRESS",
    0x000103FF: "IMPORT_LIBRARIES",
    0x00018002: "CHECKSUM_TIMESTAMP",
    0x00018102: "ENABLED_FOR_CALLCAP",
    0x00018200: "ENABLED_FOR_FASTCAP",
    0x000183FF: "ORIGINAL_PE_NAME",
    0x000200FF: "STATIC_LIBRARIES",
    0x00020104: "TLS_INFO",
    0x00020200: "DEFAULT_STACK_SIZE",
    0x00020301: "DEFAULT_FILESYSTEM_CACHE_SIZE",
    0x00020401: "DEFAULT_HEAP_SIZE",
    0x00028002: "PAGE_HEAP_SIZE_AND_FLAGS",
    0x00030000: "SYSTEM_FLAGS",
    0x00040006: "EXECUTION_INFO",
    0x00040201: "TITLE_WORKSPACE_SIZE",
    0x00040310: "GAME_RATINGS",
    0x00040404: "LAN_KEY",
    0x000405FF: "XBOX360_LOGO",
    0x000406FF: "MULTIDISC_MEDIA_IDS",
    0x000407FF: "ALTERNATE_TITLE_IDS",
    0x00040801: "ADDITIONAL_TITLE_MEMORY",
    0x00E10402: "EXPORTS_BY_NAME",
}

ENCRYPTION = {0: "none", 1: "normal"}
COMPRESSION = {0: "none", 1: "basic", 2: "normal (LZX)", 3: "delta"}
PAGE_INFO = {1: "code", 2: "data", 3: "readonly"}
SECTION_FLAGS = {
    0x00000020: "CNT_CODE", 0x00000040: "CNT_INITIALIZED_DATA", 0x00000080: "CNT_UNINITIALIZED_DATA",
    0x02000000: "MEM_DISCARDABLE", 0x10000000: "MEM_SHARED", 0x20000000: "MEM_EXECUTE",
    0x40000000: "MEM_READ", 0x80000000: "MEM_WRITE",
}


def hx(v):
    return f"0x{v:08X}"


def be32(b, o):
    return struct.unpack_from(">I", b, o)[0]


def be16(b, o):
    return struct.unpack_from(">H", b, o)[0]


def cstr(b):
    return b.split(b"\x00", 1)[0].decode("latin-1")


def fmt_version(v):
    return f"{(v >> 28) & 0xF}.{(v >> 24) & 0xF}.{(v >> 8) & 0xFFFF}.{v & 0xFF}"


def aes_ecb_decrypt(key, data):
    d = Cipher(algorithms.AES(key), modes.ECB()).decryptor()
    return d.update(data) + d.finalize()


def aes_cbc_decrypt(key, data):
    d = Cipher(algorithms.AES(key), modes.CBC(bytes(16))).decryptor()
    n = len(data) & ~15
    return d.update(data[:n]) + d.finalize() + data[n:]


class XexError(Exception):
    pass


class Xex2:
    def __init__(self, data):
        self.data = data
        if data[:4] != b"XEX2":
            raise XexError("not an XEX2 file")
        self.module_flags, self.pe_offset, _, self.sec_offset, n = struct.unpack_from(">IIIII", data, 4)
        self.opt = {}       # key -> (raw value, data bytes or None)
        self.opt_order = []
        for i in range(n):
            key, val = struct.unpack_from(">II", data, 0x18 + i * 8)
            lo = key & 0xFF
            if lo in (0x00, 0x01):
                blob = None
            elif lo == 0xFF:
                size = be32(data, val)
                blob = data[val:val + size]
            else:
                blob = data[val:val + lo * 4]
            self.opt[key] = (val, blob)
            self.opt_order.append(key)
        self._parse_security()

    # ---------------------------------------------------------------- headers
    def _parse_security(self):
        d, o = self.data, self.sec_offset
        self.sec_header_size = be32(d, o)
        self.image_size = be32(d, o + 4)
        self.image_info_size = be32(d, o + 0x108)
        self.image_flags = be32(d, o + 0x10C)
        self.load_address = be32(d, o + 0x110)
        self.section_digest = d[o + 0x114:o + 0x128]
        self.import_table_count = be32(d, o + 0x128)
        self.media_id = d[o + 0x140:o + 0x150]
        self.encrypted_key = d[o + 0x150:o + 0x160]
        self.export_table = be32(d, o + 0x160)
        self.game_regions = be32(d, o + 0x178)
        self.allowed_media = be32(d, o + 0x17C)
        count = be32(d, o + 0x180)
        self.page_size = 0x1000 if self.image_flags & 0x10000000 else 0x10000
        self.pages = []
        addr = self.load_address
        for i in range(count):
            v = be32(d, o + 0x184 + i * 24)
            npages, info = v >> 4, v & 0xF
            self.pages.append({"address": hx(addr), "size": hx(npages * self.page_size),
                               "type": PAGE_INFO.get(info, f"unknown({info})")})
            addr += npages * self.page_size

    def verify_pages(self, img):
        """Verify the page-descriptor SHA-1 chain against a decoded image.
        security.section_digest = SHA1(block[0] + desc[0]) and
        desc[i].digest          = SHA1(block[i+1] + desc[i+1]),
        where block[i] is the image range described by desc[i]."""
        d, o = self.data, self.sec_offset
        descs = [d[o + 0x184 + i * 24:o + 0x184 + i * 24 + 24] for i in range(len(self.pages))]
        expected = self.section_digest
        off = 0
        for i, desc in enumerate(descs):
            size = (be32(desc, 0) >> 4) * self.page_size
            if hashlib.sha1(img[off:off + size] + desc).digest() != expected:
                return False, i
            expected = desc[4:]
            off += size
        return True, len(descs)

    def opt_value(self, key, default=None):
        return self.opt[key][0] if key in self.opt else default

    def opt_blob(self, key):
        return self.opt[key][1] if key in self.opt else None

    def file_format(self):
        b = self.opt_blob(0x3FF)
        info_size, enc, comp = struct.unpack_from(">IHH", b, 0)
        ff = {"encryption": enc, "compression": comp}
        if comp == 1:
            blocks = []
            for o in range(8, info_size, 8):
                blocks.append(struct.unpack_from(">II", b, o))
            ff["basic_blocks"] = blocks
        elif comp == 2:
            ff["window_size"] = be32(b, 8)
            ff["first_block_size"] = be32(b, 12)
            ff["first_block_hash"] = b[16:36]
        return ff

    def import_libraries(self):
        b = self.opt_blob(0x103FF)
        if b is None:
            return []
        _size, str_size, count = struct.unpack_from(">III", b, 0)
        names = [s.decode("latin-1") for s in b[12:12 + str_size].split(b"\x00") if s]
        libs = []
        o = 12 + str_size
        for _ in range(count):
            lsize = be32(b, o)
            digest = b[o + 4:o + 24]
            lib_id, ver, ver_min = struct.unpack_from(">III", b, o + 24)
            name_idx, rec_count = struct.unpack_from(">HH", b, o + 36)
            records = list(struct.unpack_from(f">{rec_count}I", b, o + 40))
            libs.append({"name": names[name_idx], "id": lib_id, "version": ver, "min_version": ver_min,
                         "digest": digest.hex(), "records": records})
            o += lsize
        return libs

    def session_keys(self):
        return [("retail", aes_ecb_decrypt(XEX2_RETAIL_KEY, self.encrypted_key)),
                ("devkit", aes_ecb_decrypt(XEX2_DEVKIT_KEY, self.encrypted_key))]

    # ------------------------------------------------------------- payload
    def extract_image(self, verbose=True):
        ff = self.file_format()
        payload = self.data[self.pe_offset:]
        candidates = [("none", None)] if ff["encryption"] == 0 else self.session_keys()
        last_err = None
        for key_name, key in candidates:
            buf = payload if key is None else aes_cbc_decrypt(key, payload)
            try:
                img = self._decompress(buf, ff, verbose)
            except (lzx.LzxError, XexError, IndexError) as e:
                last_err = e
                continue
            if img[:2] == b"MZ":
                self.key_used = key_name
                self.session_key = key
                return img
            last_err = XexError(f"{key_name} key: output does not start with MZ")
        raise XexError(f"image extraction failed: {last_err}")

    def _decompress(self, buf, ff, verbose):
        comp = ff["compression"]
        if comp == 0:
            return bytes(buf[:self.image_size]).ljust(self.image_size, b"\0")
        if comp == 1:
            out = bytearray()
            p = 0
            for dsize, zsize in ff["basic_blocks"]:
                out += buf[p:p + dsize]
                out += bytes(zsize)
                p += dsize
            return bytes(out[:self.image_size]).ljust(self.image_size, b"\0")
        if comp == 2:
            return self._decompress_normal(buf, ff, verbose)
        raise XexError(f"unsupported compression {COMPRESSION.get(comp, comp)}")

    def _decompress_normal(self, buf, ff, verbose):
        # Each block: u32 next_block_size, sha1(next block)[20], then chunks of
        # {u16 size, data}, terminated by a zero-size chunk. Chunk payloads
        # concatenated form one continuous LZX stream.
        stream = bytearray()
        p = 0
        block_size = ff["first_block_size"]
        expected_hash = ff["first_block_hash"]
        nblocks = 0
        while block_size:
            block = buf[p:p + block_size]
            if len(block) != block_size:
                raise XexError("truncated compressed block")
            if hashlib.sha1(block).digest() != expected_hash:
                raise XexError(f"block {nblocks} SHA-1 mismatch (wrong key?)")
            next_size = be32(block, 0)
            expected_hash = block[4:24]
            q = 24
            while True:
                csize = be16(block, q)
                q += 2
                if csize == 0:
                    break
                stream += block[q:q + csize]
                q += csize
            p += block_size
            block_size = next_size
            nblocks += 1
        self.lzx_blocks = nblocks
        self.lzx_stream_size = len(stream)
        if verbose:
            print(f"[xex2] {nblocks} compressed blocks verified (SHA-1 chain OK), "
                  f"LZX stream {len(stream):,} bytes, window 0x{ff['window_size']:X}")
            print(f"[xex2] decompressing {self.image_size:,} bytes (pure Python LZX)...")
        t0 = time.time()
        img = lzx.decompress(stream, self.image_size, ff["window_size"])
        if verbose:
            print(f"[xex2] LZX done in {time.time() - t0:.1f}s")
        return img


# --------------------------------------------------------------------- PE
def parse_pe(img, load_address):
    if img[:2] != b"MZ":
        raise XexError("base image has no MZ header")
    e_lfanew = struct.unpack_from("<I", img, 0x3C)[0]
    if img[e_lfanew:e_lfanew + 4] != b"PE\0\0":
        raise XexError("no PE signature")
    machine, nsec, timestamp, _, _, opt_size, chars = struct.unpack_from("<HHIIIHH", img, e_lfanew + 4)
    oh = e_lfanew + 24
    magic = struct.unpack_from("<H", img, oh)[0]
    entry_rva = struct.unpack_from("<I", img, oh + 16)[0]
    image_base = struct.unpack_from("<I", img, oh + 28)[0]
    section_align, file_align = struct.unpack_from("<II", img, oh + 32)
    size_of_image, size_of_headers = struct.unpack_from("<II", img, oh + 56)
    subsystem = struct.unpack_from("<H", img, oh + 68)[0]
    ndirs = struct.unpack_from("<I", img, oh + 92)[0]
    dir_names = ["export", "import", "resource", "exception", "security", "basereloc", "debug",
                 "architecture", "globalptr", "tls", "load_config", "bound_import", "iat",
                 "delay_import", "com_descriptor", "reserved"]
    dirs = {}
    for i in range(min(ndirs, 16)):
        rva, size = struct.unpack_from("<II", img, oh + 96 + i * 8)
        if rva or size:
            dirs[dir_names[i]] = {"rva": hx(rva), "address": hx(load_address + rva), "size": hx(size)}
    sections = []
    so = oh + opt_size
    for i in range(nsec):
        name = img[so:so + 8].rstrip(b"\0").decode("latin-1")
        vsize, vaddr, rsize, rptr = struct.unpack_from("<IIII", img, so + 8)
        flags = struct.unpack_from("<I", img, so + 36)[0]
        sections.append({
            "name": name,
            "virtual_address": hx(load_address + vaddr),
            "rva": hx(vaddr),
            "virtual_size": hx(vsize),
            "raw_size": hx(rsize),
            "raw_offset": hx(rptr),
            "end": hx(load_address + vaddr + vsize),
            "characteristics": hx(flags),
            "flags": [n for bit, n in SECTION_FLAGS.items() if flags & bit],
        })
        so += 40
    return {
        "machine": f"0x{machine:04X}" + (" (IMAGE_FILE_MACHINE_POWERPCBE)" if machine == 0x1F2 else ""),
        "timestamp": timestamp,
        "timestamp_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(timestamp)),
        "characteristics": f"0x{chars:04X}",
        "optional_header_magic": f"0x{magic:04X}",
        "image_base": hx(image_base),
        "entry_point_rva": hx(entry_rva),
        "entry_point": hx(image_base + entry_rva),
        "section_alignment": hx(section_align),
        "file_alignment": hx(file_align),
        "size_of_image": hx(size_of_image),
        "size_of_headers": hx(size_of_headers),
        "subsystem": subsystem,
        "data_directories": dirs,
        "sections": sections,
    }


def unique_section_map(sections):
    """name -> summary; repeated names (e.g. 7x '.embsec_') get '#index' suffixes."""
    counts = {}
    for s in sections:
        counts[s["name"]] = counts.get(s["name"], 0) + 1
    seen = {}
    out = {}
    for s in sections:
        name = s["name"]
        if counts[name] > 1:
            name = f"{name}#{seen.get(s['name'], 0)}"
            seen[s["name"]] = seen.get(s["name"], 0) + 1
        out[name] = {k: s[k] for k in ("virtual_address", "end", "virtual_size", "flags")}
    return out


def parse_pdata(img, load_address, section):
    """Xbox 360 RUNTIME_FUNCTION (8 bytes, big-endian):
         u32 BeginAddress
         u32 PrologLength:8 | FunctionLength:22 (in instructions) | Flag32Bit:1 | ExceptionFlag:1"""
    start = int(section["rva"], 16)
    size = int(section["virtual_size"], 16)
    funcs = []
    for o in range(start, start + size - 7, 8):
        begin, packed = struct.unpack_from(">II", img, o)
        if begin == 0:
            break
        funcs.append({
            "address": begin,
            "length": ((packed >> 8) & 0x3FFFFF) * 4,
            "prolog": (packed & 0xFF) * 4,
            "flag32": (packed >> 30) & 1,
            "has_eh": (packed >> 31) & 1,
        })
    return funcs


def resolve_imports(img, load_address, libs):
    """Each import record is the address of a word that, before loading, holds
    (type << 24) | (library_index << 16) | ordinal; type 0 = variable slot,
    type 1 = 16-byte function thunk (patched by the loader, ends mtctr r11; bctr)."""
    out = []
    for lib_index, lib in enumerate(libs):
        by_ord = {}
        for rec in lib["records"]:
            off = rec - load_address
            value = struct.unpack_from(">I", img, off)[0]
            kind = value >> 24
            if (value >> 16) & 0xFF != lib_index:
                raise XexError(f"import record {hx(rec)} has library index {(value >> 16) & 0xFF}, expected {lib_index}")
            ordinal = value & 0xFFFF
            e = by_ord.setdefault(ordinal, {"ordinal": ordinal, "ordinal_hex": f"0x{ordinal:04X}",
                                            "variable_address": None, "thunk_address": None})
            if kind == 0:
                e["variable_address"] = hx(rec)
            elif kind == 1:
                e["thunk_address"] = hx(rec)
            else:
                e.setdefault("unknown_records", []).append(hx(rec))
        imports = sorted(by_ord.values(), key=lambda e: e["ordinal"])
        for e in imports:
            e["type"] = "function" if e["thunk_address"] else "variable"
        out.append({
            "name": lib["name"],
            "library_index": lib_index,
            "version": fmt_version(lib["version"]),
            "min_version": fmt_version(lib["min_version"]),
            "id": hx(lib["id"]),
            "digest": lib["digest"],
            "record_count": len(lib["records"]),
            "function_count": sum(1 for e in imports if e["type"] == "function"),
            "variable_count": sum(1 for e in imports if e["type"] == "variable"),
            "imports": imports,
        })
    return out


def decode_opt_headers(x):
    res = {}
    for key in x.opt_order:
        val, blob = x.opt[key]
        name = OPT_HEADER_NAMES.get(key, f"UNKNOWN_{key:08X}")
        entry = {"key": hx(key)}
        if blob is None:
            entry["value"] = hx(val)
        else:
            entry["offset"] = hx(val)
            entry["size"] = len(blob)
        b = blob
        if key == 0x2FF:
            entry["resources"] = [{"name": cstr(b[o:o + 8]), "address": hx(be32(b, o + 8)), "size": hx(be32(b, o + 12))}
                                  for o in range(4, len(b) - 15, 16)]
        elif key == 0x3FF:
            ff = x.file_format()
            entry["encryption"] = ENCRYPTION.get(ff["encryption"], ff["encryption"])
            entry["compression"] = COMPRESSION.get(ff["compression"], ff["compression"])
            if "window_size" in ff:
                entry["lzx_window_size"] = hx(ff["window_size"])
                entry["first_block_size"] = hx(ff["first_block_size"])
                entry["first_block_sha1"] = ff["first_block_hash"].hex()
            if "basic_blocks" in ff:
                entry["basic_blocks"] = len(ff["basic_blocks"])
        elif key == 0x18002:
            cs, ts = struct.unpack_from(">II", b, 0)
            entry["checksum"] = hx(cs)
            entry["timestamp"] = ts
            entry["timestamp_utc"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(ts))
        elif key in (0x183FF, 0x80FF):
            entry["string"] = cstr(b[4:])
        elif key == 0x200FF:
            libs = []
            for o in range(4, len(b) - 15, 16):
                major, minor, build, aq = struct.unpack_from(">HHHH", b, o + 8)
                libs.append({"name": cstr(b[o:o + 8]), "version": f"{major}.{minor}.{build}.{aq & 0x1FFF}",
                             "approval": (aq >> 13) & 3})
            entry["libraries"] = libs
        elif key == 0x20104:
            sc, rda, ds, rds = struct.unpack_from(">IIII", b, 0)
            entry.update(slot_count=sc, raw_data_address=hx(rda), data_size=hx(ds), raw_data_size=hx(rds))
        elif key == 0x40006:
            media, ver, bver, tid = struct.unpack_from(">IIII", b, 0)
            plat, et, dn, dc = struct.unpack_from(">BBBB", b, 16)
            sg = be32(b, 20)
            entry.update(media_id=hx(media), version=fmt_version(ver), base_version=fmt_version(bver),
                         title_id=hx(tid), platform=plat, executable_type=et, disc_number=dn,
                         disc_count=dc, savegame_id=hx(sg))
        elif key == 0x40310:
            entry["ratings"] = b.hex()
        elif key == 0x103FF:
            entry["libraries"] = [l["name"] for l in x.import_libraries()]
        res[name] = entry
    return res


def main():
    ap = argparse.ArgumentParser(description="Parse an XEX2 and extract its PE base image")
    ap.add_argument("xex", help="path to default.xex")
    ap.add_argument("-o", "--out", default="extracted/base_image.bin", help="output base image")
    ap.add_argument("-j", "--json", default="analysis/binary_map.json", help="output JSON map")
    ap.add_argument("--pdata-json", default="analysis/pdata_functions.json",
                    help="output JSON with the full .pdata function table")
    args = ap.parse_args()

    data = open(args.xex, "rb").read()
    x = Xex2(data)
    ff = x.file_format()
    print(f"[xex2] {args.xex}: {len(data):,} bytes, module flags 0x{x.module_flags:08X}, "
          f"{len(x.opt)} optional headers")
    print(f"[xex2] load address {hx(x.load_address)}, image size 0x{x.image_size:X}, "
          f"entry {hx(x.opt_value(0x10100, 0))}")
    print(f"[xex2] encryption={ENCRYPTION.get(ff['encryption'])}, compression={COMPRESSION.get(ff['compression'])}")

    img = x.extract_image()
    print(f"[xex2] decrypted with {x.key_used} key; base image {len(img):,} bytes, "
          f"sha1 {hashlib.sha1(img).hexdigest()}")
    pages_ok, pages_n = x.verify_pages(img)
    if not pages_ok:
        raise XexError(f"page SHA-1 chain mismatch at descriptor {pages_n}: decoded image is corrupt")
    print(f"[xex2] page SHA-1 chain verified over {pages_n} descriptors (byte-exact image)")
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "wb") as f:
        f.write(img)

    pe = parse_pe(img, x.load_address)
    libs = resolve_imports(img, x.load_address, x.import_libraries())
    pdata_sec = next((s for s in pe["sections"] if s["name"] == ".pdata"), None)
    funcs = parse_pdata(img, x.load_address, pdata_sec) if pdata_sec else []
    text_sec = next((s for s in pe["sections"] if s["name"] == ".text"), None)

    entry = x.opt_value(0x10100)
    entry_func = next((f for f in funcs if f["address"] <= entry < f["address"] + f["length"]), None)
    exec_info = decode_opt_headers(x)

    report = {
        "source": os.path.basename(args.xex),
        "source_size": len(data),
        "source_sha1": hashlib.sha1(data).hexdigest(),
        "format": "XEX2",
        "module_flags": hx(x.module_flags),
        "pe_data_offset": hx(x.pe_offset),
        "security_info_offset": hx(x.sec_offset),
        "load_address": hx(x.load_address),
        "image_base_address": hx(x.opt_value(0x10201, x.load_address)),
        "entry_point": hx(entry),
        "entry_point_in_pdata_function": hx(entry_func["address"]) if entry_func else None,
        "image_size": hx(x.image_size),
        "image_flags": hx(x.image_flags),
        "page_size": hx(x.page_size),
        "game_regions": hx(x.game_regions),
        "allowed_media_types": hx(x.allowed_media),
        "export_table": hx(x.export_table),
        "media_id": x.media_id.hex(),
        "encryption": {
            "type": ENCRYPTION.get(ff["encryption"]),
            "key_used": x.key_used,
            "cipher": "AES-128-CBC, IV=0 (session key = AES-128-ECB-decrypt(xex2_key, encrypted_session_key))",
        },
        "compression": {
            "type": COMPRESSION.get(ff["compression"]),
            "lzx_window_size": hx(ff["window_size"]) if "window_size" in ff else None,
            "lzx_blocks": getattr(x, "lzx_blocks", None),
            "lzx_stream_size": getattr(x, "lzx_stream_size", None),
        },
        "base_image": {
            "path": args.out.replace("\\", "/"),
            "size": len(img),
            "sha1": hashlib.sha1(img).hexdigest(),
            "page_hash_chain_verified": pages_ok,
            "page_descriptors": pages_n,
            "note": "PE .reloc/.XBLD/.idata headers are stale: the XEX resources (XDBF at "
                    "RESOURCE_INFO) occupy 0x82C1F780+ and .reloc extends past the image end; "
                    "relocations were stripped because the image loads at its fixed base.",
        },
        "optional_headers": exec_info,
        "memory_pages": x.pages,
        "pe": pe,
        "sections": unique_section_map(pe["sections"]),
        "pdata": {
            "function_count": len(funcs),
            "first_function": hx(funcs[0]["address"]) if funcs else None,
            "last_function": hx(funcs[-1]["address"]) if funcs else None,
            "covered_code_bytes": sum(f["length"] for f in funcs),
            "text_size": int(text_sec["virtual_size"], 16) if text_sec else None,
            "full_table": args.pdata_json.replace("\\", "/"),
        },
        "imports": libs,
        "import_summary": {l["name"]: {"functions": l["function_count"], "variables": l["variable_count"]}
                           for l in libs},
    }
    os.makedirs(os.path.dirname(os.path.abspath(args.json)), exist_ok=True)
    with open(args.json, "w", encoding="utf-8") as f:
        json.dump(report, f, indent=2)
    with open(args.pdata_json, "w", encoding="utf-8") as f:
        json.dump([{"address": hx(fn["address"]), "length": fn["length"], "prolog": fn["prolog"],
                    "has_eh": fn["has_eh"]} for fn in funcs], f, indent=0)

    print(f"[xex2] PE: machine {pe['machine']}, image base {pe['image_base']}, entry {pe['entry_point']}")
    for s in pe["sections"]:
        print(f"         {s['name']:<10} {s['virtual_address']}-{s['end']} size {s['virtual_size']} {','.join(s['flags'])}")
    for l in libs:
        print(f"[xex2] import {l['name']} v{l['version']}: {l['function_count']} functions, {l['variable_count']} variables")
    print(f"[xex2] .pdata: {len(funcs)} functions")
    print(f"[xex2] wrote {args.out} and {args.json}")


if __name__ == "__main__":
    sys.exit(main())
