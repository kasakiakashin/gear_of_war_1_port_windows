#!/usr/bin/env python3
"""recomp_difftest.py — teste diferencial da fase 2.

Escolhe rotinas-folha reais do binário (sem chamadas, só instruções inteiras/memória), executa-as
num interpretador PowerPC de referência escrito aqui (independente do gerador de C++) e grava os
resultados esperados. O `ppc_smoke --difftest <ficheiro>` executa as mesmas rotinas já traduzidas
para C++ com as mesmas entradas e compara r3 e um hash da área de teste.

Uso: python tools/recomp_difftest.py -o analysis/difftest_cases.txt [--max 300]
"""
import argparse
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ppc_recomp import decode, sext, mask64  # só o decodificador de mnemónicos é partilhado

M64 = (1 << 64) - 1
TEST_BASE, TEST_SIZE = 0x10000, 0x10000
STACK_TOP = 0x1F000
INPUTS = [  # (r3, r4, r5, r6)
    (0x10000, 0x10800, 7, 3),
    (0x10040, 0x10400, 0, 0x7FFFFFFF),
    (0x10123, 0x10900, 0xFFFFFFFFFFFFFFFF, 100),
]
TEXT = b"Gears of War recompiled from PowerPC to C++20\x00"


class Unsupported(Exception):
    pass


def make_test_area():
    area = bytearray(((i * 7 + 3) & 0xFF) for i in range(TEST_SIZE))
    area[0:len(TEXT)] = TEXT
    area[0x800:0x800 + len(TEXT)] = TEXT[::-1][1:] + b"\x00"
    return area


def fnv1a64(data):
    h = 0xCBF29CE484222325
    for b in data:
        h = ((h ^ b) * 0x100000001B3) & M64
    return h


class Interp:
    def __init__(self, img, img_base):
        self.img, self.img_base = img, img_base

    def run(self, entry, end, regs_in, max_steps=20000):
        r = [0] * 32
        r[1] = STACK_TOP
        r[3:7] = list(regs_in)
        cr = [0] * 8  # nibble lt gt eq so
        ca = 0
        ctr = 0
        area = make_test_area()
        img, base = self.img, self.img_base

        def mem_read(ea, n):
            ea &= 0xFFFFFFFF
            if TEST_BASE <= ea and ea + n <= TEST_BASE + TEST_SIZE:
                return int.from_bytes(area[ea - TEST_BASE: ea - TEST_BASE + n], "big")
            if base <= ea and ea + n <= base + len(img):
                return int.from_bytes(img[ea - base: ea - base + n], "big")
            raise Unsupported("leitura fora")

        def mem_write(ea, n, v):
            ea &= 0xFFFFFFFF
            if TEST_BASE <= ea and ea + n <= TEST_BASE + TEST_SIZE:
                area[ea - TEST_BASE: ea - TEST_BASE + n] = (v & ((1 << (8 * n)) - 1)).to_bytes(n, "big")
                return
            raise Unsupported("escrita fora")

        def setcr(f, a, b):
            cr[f] = (8 if a < b else 4 if a > b else 2)

        def s64(v):
            v &= M64
            return v - (1 << 64) if v >> 63 else v

        def s32(v):
            v &= 0xFFFFFFFF
            return v - (1 << 32) if v >> 31 else v

        def rc0(v):  # forma com ponto: CR0 dos 32 bits baixos (como o XenonRecomp/Xenia)
            setcr(0, s32(v), 0)

        def rotl32(v, n):
            v &= 0xFFFFFFFF
            x = v | (v << 32)
            n &= 63
            return ((x << n) | (x >> (64 - n))) & M64 if n else x

        def rotl64(v, n):
            n &= 63
            return ((v << n) | (v >> (64 - n))) & M64 if n else v

        pc = entry
        for _ in range(max_steps):
            if not (entry <= pc < end):
                raise Unsupported("saiu da rotina")
            w = struct.unpack_from(">I", img, pc - base)[0]
            name = decode(w)
            d, a, b = (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
            simm = sext(w & 0xFFFF, 16)
            uimm = w & 0xFFFF
            rc = w & 1
            ra0 = 0 if a == 0 else r[a]
            npc = pc + 4
            if name == "addi":
                r[d] = (ra0 + simm) & M64
            elif name == "addis":
                r[d] = (ra0 + (simm << 16)) & M64
            elif name == "add":
                r[d] = (r[a] + r[b]) & M64
                if rc: rc0(r[d])
            elif name == "subf":
                r[d] = (r[b] - r[a]) & M64
                if rc: rc0(r[d])
            elif name == "neg":
                r[d] = (-r[a]) & M64
                if rc: rc0(r[d])
            elif name == "mulli":
                r[d] = (r[a] * simm) & M64
            elif name == "mullw":
                r[d] = (s32(r[a]) * s32(r[b])) & M64
                if rc: rc0(r[d])
            elif name in ("divw", "divwu"):
                x, y = (s32(r[a]), s32(r[b])) if name == "divw" else (r[a] & 0xFFFFFFFF, r[b] & 0xFFFFFFFF)
                if y == 0 or (name == "divw" and x == -2 ** 31 and y == -1):
                    q = 0
                else:
                    q = abs(x) // abs(y) * (1 if (x < 0) == (y < 0) else -1)
                r[d] = q & 0xFFFFFFFF
                if rc: rc0(r[d])
            elif name in ("or", "and", "andc", "xor", "nor", "orc", "nand", "eqv"):
                x, y = r[d], r[b]
                v = {"or": x | y, "and": x & y, "andc": x & ~y, "xor": x ^ y, "nor": ~(x | y),
                     "orc": x | ~y, "nand": ~(x & y), "eqv": ~(x ^ y)}[name] & M64
                r[a] = v
                if rc: rc0(v)
            elif name in ("ori", "oris", "xori", "xoris", "andi.", "andis."):
                imm = uimm << 16 if name.endswith(("is", "is.")) else uimm
                v = {"o": r[d] | imm, "x": r[d] ^ imm, "a": r[d] & imm}[name[0]]
                r[a] = v
                if name.startswith("and"): rc0(v)
            elif name in ("rlwinm", "rlwimi", "rlwnm"):
                sh = (r[b] & 31) if name == "rlwnm" else b
                mb, me = (w >> 6) & 31, (w >> 1) & 31
                m = mask64(mb + 32, me + 32)
                v = rotl32(r[d], sh) & m
                if name == "rlwimi":
                    v |= r[a] & ~m & M64
                r[a] = v
                if rc: rc0(v)
            elif name in ("rldicl", "rldicr"):
                sh = b | (((w >> 1) & 1) << 5)
                mbx = ((w >> 6) & 31) | (((w >> 5) & 1) << 5)
                m = mask64(mbx, 63) if name == "rldicl" else mask64(0, mbx)
                r[a] = rotl64(r[d], sh) & m
                if rc: rc0(r[a])
            elif name in ("slw", "srw"):
                n = r[b] & 63
                x = r[d] & 0xFFFFFFFF
                r[a] = 0 if n > 31 else ((x << n) & 0xFFFFFFFF if name == "slw" else x >> n)
                if rc: rc0(r[a])
            elif name in ("sraw", "srawi"):
                n = b if name == "srawi" else (r[b] & 63)
                x = s32(r[d])
                if n > 31:
                    v, ca = (-1 if x < 0 else 0), int(x < 0)
                else:
                    v = x >> n
                    ca = int(x < 0 and (x & ((1 << n) - 1)) != 0)
                r[a] = v & M64
                if rc: rc0(r[a])
            elif name == "cntlzw":
                x = r[d] & 0xFFFFFFFF
                r[a] = 32 - x.bit_length()
                if rc: rc0(r[a])
            elif name in ("extsb", "extsh", "extsw"):
                bits = {"extsb": 8, "extsh": 16, "extsw": 32}[name]
                r[a] = sext(r[d] & ((1 << bits) - 1), bits) & M64
                if rc: rc0(r[a])
            elif name in ("subfic", "addic", "addic."):
                x = (~r[a] & M64) + (simm & M64) + 1 if name == "subfic" else r[a] + (simm & M64)
                ca = int(x > M64)
                r[d] = x & M64
                if name == "addic.": rc0(r[d])
            elif name in ("addze", "adde", "subfe", "subfze", "addc", "subfc"):
                x = r[a] if name in ("addze", "adde", "addc") else (~r[a] & M64)
                y = {"addze": 0, "subfze": 0}.get(name, r[b])
                cin = 1 if name == "subfc" else 0 if name == "addc" else ca
                s = x + y + cin
                ca = int(s > M64)
                r[d] = s & M64
                if rc: rc0(r[d])
            elif name in ("cmpi", "cmpli", "cmp", "cmpl"):
                f, l = d >> 2, d & 1
                if name in ("cmpi", "cmp"):
                    x = s64(r[a]) if l else s32(r[a])
                    y = simm if name == "cmpi" else (s64(r[b]) if l else s32(r[b]))
                else:
                    x = r[a] if l else r[a] & 0xFFFFFFFF
                    y = uimm if name == "cmpli" else (r[b] if l else r[b] & 0xFFFFFFFF)
                setcr(f, x, y)
            elif name in ("b", "bc"):
                if w & 1:
                    raise Unsupported("chamada")
                if name == "b":
                    take, tgt = True, (pc + sext(w & 0x03FFFFFC, 26)) & 0xFFFFFFFF
                else:
                    bo, bi = d, a
                    if not (bo & 4):
                        ctr = (ctr - 1) & M64
                    ok_ctr = (bo & 4) or (((ctr & 0xFFFFFFFF) != 0) != bool(bo & 2))
                    bit = (cr[bi >> 2] >> (3 - (bi & 3))) & 1
                    ok_c = (bo & 16) or (bit == ((bo >> 3) & 1))
                    take, tgt = ok_ctr and ok_c, (pc + sext(w & 0xFFFC, 16)) & 0xFFFFFFFF
                if w & 2:
                    raise Unsupported("absoluto")
                if take:
                    npc = tgt
            elif name == "bclr":
                if w & 1:
                    raise Unsupported("blrl")
                bo, bi = d, a
                if not (bo & 4):
                    ctr = (ctr - 1) & M64
                ok_ctr = (bo & 4) or (((ctr & 0xFFFFFFFF) != 0) != bool(bo & 2))
                bit = (cr[bi >> 2] >> (3 - (bi & 3))) & 1
                ok_c = (bo & 16) or (bit == ((bo >> 3) & 1))
                if ok_ctr and ok_c:
                    return r[3], fnv1a64(area)
            elif name in ("mfspr", "mtspr"):
                spr = ((w >> 16) & 31) | (((w >> 11) & 31) << 5)
                if spr != 9:
                    raise Unsupported("spr")
                if name == "mtspr":
                    ctr = r[d]
                else:
                    r[d] = ctr
            elif name in ("lbz", "lhz", "lha", "lwz", "lbzu", "lhzu", "lwzu", "lbzx", "lhzx", "lwzx", "lhax"):
                n = {"b": 1, "h": 2, "w": 4}[name[1]]
                ea = ((ra0 + r[b]) if name.endswith("x") else (ra0 + simm)) & 0xFFFFFFFF
                v = mem_read(ea, n)
                if name.startswith("lha"):
                    v = sext(v, 16) & M64
                r[d] = v
                if name.endswith("u"):
                    r[a] = ea
            elif name in ("stb", "sth", "stw", "stbu", "sthu", "stwu", "stbx", "sthx", "stwx", "std", "ld"):
                if name in ("std", "ld"):
                    ea = (ra0 + sext(w & 0xFFFC, 16)) & 0xFFFFFFFF
                    if w & 3:
                        raise Unsupported("ds")
                    if name == "ld":
                        r[d] = mem_read(ea, 8)
                    else:
                        mem_write(ea, 8, r[d])
                else:
                    n = {"b": 1, "h": 2, "w": 4}[name[2]]
                    ea = ((ra0 + r[b]) if name.endswith("x") else (ra0 + simm)) & 0xFFFFFFFF
                    mem_write(ea, n, r[d])
                    if name.endswith("u"):
                        r[a] = ea
            else:
                raise Unsupported(name)
            pc = npc
        raise Unsupported("passos")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--image", default="extracted/base_image.bin")
    ap.add_argument("--pdata", default="analysis/pdata_functions.json")
    ap.add_argument("-o", "--out", default="analysis/difftest_cases.txt")
    ap.add_argument("--max", type=int, default=300)
    args = ap.parse_args()
    img = open(args.image, "rb").read()
    base = 0x82000000
    funcs = [(int(f["address"], 16), f["length"]) for f in json.load(open(args.pdata))]
    interp = Interp(img, base)
    cases, tried = [], 0
    for start, length in funcs:
        if len(cases) >= args.max:
            break
        if length > 4 * 96:
            continue
        tried += 1
        rows = []
        try:
            for regs in INPUTS:
                r3, h = interp.run(start, start + length, regs)
                rows.append((start, *regs, r3, h))
        except (Unsupported, IndexError, KeyError, ValueError):
            continue
        cases.extend(rows)
    with open(args.out, "w") as fp:
        fp.write(f"# addr r3 r4 r5 r6 -> r3 fnv1a64(area 0x{TEST_BASE:X}+0x{TEST_SIZE:X})\n")
        for row in cases:
            fp.write(" ".join(f"0x{v:X}" for v in row) + "\n")
    nfun = len({c[0] for c in cases})
    print(f"{nfun} rotinas-folha ({len(cases)} casos) de {tried} candidatas -> {args.out}")


if __name__ == "__main__":
    main()
