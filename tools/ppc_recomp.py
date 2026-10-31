#!/usr/bin/env python3
"""ppc_recomp.py — tradutor estático AOT de PowerPC 64-bit (Xenon, VMX/VMX128) para C++20.

Lê a imagem PE descomprimida (extracted/base_image.bin), o mapa da fase 1 (analysis/binary_map.json,
analysis/pdata_functions.json) e gera em generated_src/:

  ppc_context.h        estado da CPU + primitivas (copiado de tools/recomp_runtime/)
  ppc_funcs.h          declarações de todas as rotinas traduzidas
  ppc_recomp_NNN.cpp   corpo das rotinas (dividido em vários ficheiros)
  ppc_imports.cpp      stubs dos thunks de import (xam/xboxkrnl por ordinal)
  ppc_func_table.cpp   tabela de despacho {endereço convidado -> função host} para bctr/ponteiros
  ppc_runtime.cpp      implementação padrão dos ganchos do runtime (substituída na fase 3)
  ppc_smoke_main.cpp   executável de teste (carrega a imagem e valida a tabela de despacho)
  CMakeLists.txt + toolchain/  build com Ninja e `python -m ziglang c++`

Uso:
  python tools/ppc_recomp.py --image extracted/base_image.bin --map analysis/binary_map.json \
      --pdata analysis/pdata_functions.json -o generated_src --report analysis/recomp_report.json
"""
import argparse
import bisect
import collections
import json
import os
import shutil
import struct
import sys
import time

M64 = (1 << 64) - 1
CRB = ("lt", "gt", "eq", "so")


# =============================================================================================
# Campos das instruções (numeração de bits LSB = 0)
# =============================================================================================
def f_d(w): return (w >> 21) & 31
def f_a(w): return (w >> 16) & 31
def f_b(w): return (w >> 11) & 31
def f_c(w): return (w >> 6) & 31
def f_rc(w): return w & 1
def f_simm(w):
    v = w & 0xFFFF
    return v - 0x10000 if v & 0x8000 else v
def f_uimm(w): return w & 0xFFFF
def f_ds(w):
    v = w & 0xFFFC
    return v - 0x10000 if v & 0x8000 else v
def sext(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v

# VMX128
def v128_d(w): return ((w >> 21) & 31) | (((w >> 2) & 3) << 5)
def v128_a(w): return ((w >> 16) & 31) | (((w >> 5) & 1) << 5) | (((w >> 10) & 1) << 6)
def v128_b(w): return ((w >> 11) & 31) | ((w & 3) << 5)


def mask64(mb, me):
    def ones_from(x):  # bits x..63 (numeração IBM, 0 = MSB)
        return (1 << (64 - x)) - 1 if x < 64 else 0
    if mb <= me:
        return ones_from(mb) & ~ones_from(me + 1) & M64
    return (ones_from(mb) | (~ones_from(me + 1) & M64)) & M64


def hx(v):
    return f"0x{v & M64:X}ull"


# =============================================================================================
# Tabelas de decodificação
# =============================================================================================
PRIMARY = {
    2: "tdi", 3: "twi", 7: "mulli", 8: "subfic", 10: "cmpli", 11: "cmpi", 12: "addic", 13: "addic.",
    14: "addi", 15: "addis", 16: "bc", 17: "sc", 18: "b", 20: "rlwimi", 21: "rlwinm", 23: "rlwnm",
    24: "ori", 25: "oris", 26: "xori", 27: "xoris", 28: "andi.", 29: "andis.",
    32: "lwz", 33: "lwzu", 34: "lbz", 35: "lbzu", 36: "stw", 37: "stwu", 38: "stb", 39: "stbu",
    40: "lhz", 41: "lhzu", 42: "lha", 43: "lhau", 44: "sth", 45: "sthu", 46: "lmw", 47: "stmw",
    48: "lfs", 49: "lfsu", 50: "lfd", 51: "lfdu", 52: "stfs", 53: "stfsu", 54: "stfd", 55: "stfdu",
}
OP19 = {0: "mcrf", 16: "bclr", 33: "crnor", 50: "rfi", 129: "crandc", 150: "isync", 193: "crxor",
        225: "crnand", 257: "crand", 289: "creqv", 417: "crorc", 449: "cror", 528: "bcctr"}
OP31_X = {
    0: "cmp", 4: "tw", 6: "lvsl", 7: "lvebx", 19: "mfcr", 20: "lwarx", 21: "ldx", 23: "lwzx", 24: "slw",
    26: "cntlzw", 27: "sld", 28: "and", 32: "cmpl", 38: "lvsr", 39: "lvehx", 53: "ldux", 54: "dcbst",
    55: "lwzux", 58: "cntlzd", 60: "andc", 68: "td", 71: "lvewx", 83: "mfmsr", 84: "ldarx", 86: "dcbf",
    87: "lbzx", 103: "lvx", 119: "lbzux", 124: "nor", 135: "stvebx", 144: "mtcrf", 146: "mtmsr",
    149: "stdx", 150: "stwcx.", 151: "stwx", 167: "stvehx", 178: "mtmsrd", 181: "stdux", 183: "stwux",
    199: "stvewx", 214: "stdcx.", 215: "stbx", 231: "stvx", 246: "dcbtst", 247: "stbux", 278: "dcbt",
    279: "lhzx", 284: "eqv", 311: "lhzux", 316: "xor", 339: "mfspr", 341: "lwax", 343: "lhax",
    359: "lvxl", 371: "mftb", 373: "lwaux", 375: "lhaux", 407: "sthx", 412: "orc", 439: "sthux",
    444: "or", 467: "mtspr", 470: "dcbi", 476: "nand", 487: "stvxl", 519: "lvlx", 532: "ldbrx",
    534: "lwbrx", 535: "lfsx", 536: "srw", 539: "srd", 551: "lvrx", 567: "lfsux", 597: "lswi",
    598: "sync", 599: "lfdx", 631: "lfdux", 647: "stvlx", 660: "stdbrx", 662: "stwbrx", 663: "stfsx",
    679: "stvrx", 695: "stfsux", 725: "stswi", 727: "stfdx", 759: "stfdux", 775: "lvlxl", 790: "lhbrx",
    792: "sraw", 794: "srad", 807: "lvrxl", 824: "srawi", 826: "sradi", 827: "sradi", 854: "eieio",
    903: "stvlxl", 918: "sthbrx", 922: "extsh", 935: "stvrxl", 954: "extsb", 982: "icbi", 983: "stfiwx",
    986: "extsw", 1014: "dcbz",
}
OP31_XO = {8: "subfc", 9: "mulhdu", 10: "addc", 11: "mulhwu", 40: "subf", 73: "mulhd", 75: "mulhw",
           104: "neg", 136: "subfe", 138: "adde", 200: "subfze", 202: "addze", 232: "subfme",
           233: "mulld", 234: "addme", 235: "mullw", 266: "add", 457: "divdu", 459: "divwu",
           489: "divd", 491: "divw"}
OP59 = {18: "fdivs", 20: "fsubs", 21: "fadds", 22: "fsqrts", 24: "fres", 25: "fmuls", 28: "fmsubs",
        29: "fmadds", 30: "fnmsubs", 31: "fnmadds"}
OP63_A = {18: "fdiv", 20: "fsub", 21: "fadd", 22: "fsqrt", 23: "fsel", 25: "fmul", 26: "frsqrte",
          28: "fmsub", 29: "fmadd", 30: "fnmsub", 31: "fnmadd"}
OP63_X = {0: "fcmpu", 12: "frsp", 14: "fctiw", 15: "fctiwz", 32: "fcmpo", 38: "mtfsb1", 40: "fneg",
          64: "mcrfs", 70: "mtfsb0", 72: "fmr", 134: "mtfsfi", 136: "fnabs", 264: "fabs", 583: "mffs",
          711: "mtfsf", 814: "fctid", 815: "fctidz", 846: "fcfid"}

# VMX (opcode 4)
OP4_VX = {
    0: "vaddubm", 2: "vmaxub", 4: "vrlb", 8: "vmuloub", 10: "vaddfp", 12: "vmrghb", 14: "vpkuhum",
    64: "vadduhm", 66: "vmaxuh", 68: "vrlh", 72: "vmulouh", 74: "vsubfp", 76: "vmrghh", 78: "vpkuwum",
    128: "vadduwm", 130: "vmaxuw", 132: "vrlw", 140: "vmrghw", 142: "vpkuhus", 206: "vpkuwus",
    258: "vmaxsb", 260: "vslb", 264: "vmulosb", 266: "vrefp", 268: "vmrglb", 270: "vpkshus",
    322: "vmaxsh", 324: "vslh", 328: "vmulosh", 330: "vrsqrtefp", 332: "vmrglh", 334: "vpkswus",
    384: "vaddcuw", 386: "vmaxsw", 388: "vslw", 394: "vexptefp", 396: "vmrglw", 398: "vpkshss",
    452: "vsl", 458: "vlogefp", 462: "vpkswss", 512: "vaddubs", 514: "vminub", 516: "vsrb",
    520: "vmuleub", 522: "vrfin", 524: "vspltb", 526: "vupkhsb", 576: "vadduhs", 578: "vminuh",
    580: "vsrh", 584: "vmuleuh", 586: "vrfiz", 588: "vsplth", 590: "vupkhsh", 640: "vadduws",
    642: "vminuw", 644: "vsrw", 650: "vrfip", 652: "vspltw", 654: "vupklsb", 708: "vsr", 714: "vrfim",
    718: "vupklsh", 768: "vaddsbs", 770: "vminsb", 772: "vsrab", 776: "vmulesb", 778: "vcfux",
    780: "vspltisb", 782: "vpkpx", 832: "vaddshs", 834: "vminsh", 836: "vsrah", 840: "vmulesh",
    842: "vcfsx", 844: "vspltish", 846: "vupkhpx", 896: "vaddsws", 898: "vminsw", 900: "vsraw",
    906: "vctuxs", 908: "vspltisw", 970: "vctsxs", 974: "vupklpx", 1024: "vsububm", 1026: "vavgub",
    1028: "vand", 1034: "vmaxfp", 1036: "vslo", 1088: "vsubuhm", 1090: "vavguh", 1092: "vandc",
    1098: "vminfp", 1100: "vsro", 1152: "vsubuwm", 1154: "vavguw", 1156: "vor", 1220: "vxor",
    1282: "vavgsb", 1284: "vnor", 1346: "vavgsh", 1408: "vsubcuw", 1410: "vavgsw", 1536: "vsububs",
    1540: "mfvscr", 1544: "vsum4ubs", 1600: "vsubuhs", 1604: "mtvscr", 1608: "vsum4shs",
    1664: "vsubuws", 1672: "vsum2sws", 1792: "vsubsbs", 1800: "vsum4sbs", 1856: "vsubshs",
    1920: "vsubsws", 1928: "vsumsws",
}
OP4_VXR = {6: "vcmpequb", 70: "vcmpequh", 134: "vcmpequw", 198: "vcmpeqfp", 454: "vcmpgefp",
           518: "vcmpgtub", 582: "vcmpgtuh", 646: "vcmpgtuw", 710: "vcmpgtfp", 774: "vcmpgtsb",
           838: "vcmpgtsh", 902: "vcmpgtsw", 966: "vcmpbfp"}
OP4_VA = {32: "vmhaddshs", 33: "vmhraddshs", 34: "vmladduhm", 36: "vmsumubm", 37: "vmsummbm",
          38: "vmsumuhm", 39: "vmsumuhs", 40: "vmsumshm", 41: "vmsumshs", 42: "vsel", 43: "vperm",
          44: "vsldoi", 46: "vmaddfp", 47: "vnmsubfp"}
OP4_VX128_1 = {3: "lvsl128", 67: "lvsr128", 131: "lvewx128", 195: "lvx128", 387: "stvewx128",
               451: "stvx128", 707: "lvxl128", 963: "stvxl128", 1027: "lvlx128", 1091: "lvrx128",
               1283: "stvlx128", 1347: "stvrx128", 1539: "lvlxl128", 1603: "lvrxl128",
               1795: "stvlxl128", 1859: "stvrxl128"}
OP5_VX128 = {16: "vaddfp128", 80: "vsubfp128", 144: "vmulfp128", 208: "vmaddfp128",
             272: "vmaddcfp128", 336: "vnmsubfp128", 400: "vmsum3fp128", 464: "vmsum4fp128",
             512: "vpkshss128", 528: "vand128", 576: "vpkshus128", 592: "vandc128", 640: "vpkswss128",
             656: "vnor128", 704: "vpkswus128", 720: "vor128", 768: "vpkuhum128", 784: "vxor128",
             832: "vpkuhus128", 848: "vsel128", 896: "vpkuwum128", 912: "vslo128", 960: "vpkuwus128",
             976: "vsro128"}
OP6_VX128_3 = {560: "vcfpsxws128", 624: "vcfpuxws128", 688: "vcsxwfp128", 752: "vcuxwfp128",
               816: "vrfim128", 880: "vrfin128", 944: "vrfip128", 1008: "vrfiz128", 1584: "vrefp128",
               1648: "vrsqrtefp128", 1712: "vexptefp128", 1776: "vlogefp128", 1840: "vspltw128",
               1904: "vspltisw128", 2032: "vupkd3d128"}
OP6_VX128_4 = {1552: "vpkd3d128", 1808: "vrlimi128"}
OP6_VX128 = {0: "vcmpeqfp128", 64: "vcmpeqfp128.", 80: "vrlw128", 128: "vcmpgefp128",
             192: "vcmpgefp128.", 208: "vslw128", 256: "vcmpgtfp128", 320: "vcmpgtfp128.",
             336: "vsraw128", 384: "vcmpbfp128", 448: "vcmpbfp128.", 464: "vsrw128",
             512: "vcmpequw128", 576: "vcmpequw128.", 640: "vmaxfp128", 704: "vminfp128",
             768: "vmrghw128", 832: "vmrglw128", 896: "vupkhsb128", 960: "vupklsb128"}


def decode(w):
    """Devolve o mnemónico (str) ou None se a instrução não for reconhecida."""
    op = w >> 26
    n = PRIMARY.get(op)
    if n:
        return n
    if op == 19:
        return OP19.get((w >> 1) & 0x3FF)
    if op == 31:
        n = OP31_X.get((w >> 1) & 0x3FF)
        if n:
            return n
        return OP31_XO.get((w >> 1) & 0x1FF)
    if op == 30:
        x = (w >> 2) & 7
        if x < 4:
            return ("rldicl", "rldicr", "rldic", "rldimi")[x]
        x = (w >> 1) & 15
        return {8: "rldcl", 9: "rldcr"}.get(x)
    if op == 58:
        return {0: "ld", 1: "ldu", 2: "lwa"}.get(w & 3)
    if op == 62:
        return {0: "std", 1: "stdu"}.get(w & 3)
    if op == 59:
        return OP59.get((w >> 1) & 0x1F)
    if op == 63:
        x5 = (w >> 1) & 0x1F
        if x5 in OP63_A:
            return OP63_A[x5]
        return OP63_X.get((w >> 1) & 0x3FF)
    if op == 4:
        if (w & 3) == 3 and not (w & 0x20):
            return OP4_VX128_1.get(w & 0x7F3)
        if w & 0x10:
            return "vsldoi128"
        if w & 0x20:
            return OP4_VA.get(w & 0x3F)
        n = OP4_VX.get(w & 0x7FF)
        if n:
            return n
        n = OP4_VXR.get(w & 0x3FF)
        if n:
            return n + ("." if w & 0x400 else "")
        return None
    if op == 5:
        if (w & 0x210) == 0:
            return "vperm128"
        return OP5_VX128.get(w & 0x3D0)
    if op == 6:
        n = OP6_VX128_3.get(w & 0x7F0)
        if n:
            return n
        n = OP6_VX128_4.get(w & 0x730)
        if n:
            return n
        if (w & 0x630) == 0x210:
            return "vpermwi128"
        return OP6_VX128.get(w & 0x3D0)
    return None


# =============================================================================================
# Emissão de C++ por instrução
# =============================================================================================
def R(n): return f"ctx.r[{n}]"
def FR(n): return f"ctx.f[{n}]"
def VR(n): return f"ctx.v[{n}]"

def ea_d(a, d):
    if a == 0:
        return f"0x{d & 0xFFFFFFFF:X}u"
    return f"(uint32_t)(ctx.r[{a}] + {d}ll)" if d else f"(uint32_t)ctx.r[{a}]"

def ea_x(a, b):
    if a == 0:
        return f"(uint32_t)ctx.r[{b}]"
    return f"(uint32_t)(ctx.r[{a}] + ctx.r[{b}])"

# Formas com ponto (Rc=1): CR0 compara os 32 bits baixos do resultado com sinal, como o XenonRecomp e o Xenia.
# O MSVC do X360 usa "subf." para comparar ints carregados com lwz (estendidos com zeros): com a comparação de 64
# bits, 0 - 0xFFFFFFFF dava negativo e o -1 ordenava depois do 0 (TabIndex no foco do menu principal).
def cr0(a):
    return f" ppc_cmp_s(ctx.cr[0], (int64_t)(int32_t)ctx.r[{a}], 0, ctx.xer);"

LOADS = {  # nome: (tamanho, tipo, update, forma)
    "lbz": (1, "u", 0, "d"), "lbzu": (1, "u", 1, "d"), "lhz": (2, "u", 0, "d"), "lhzu": (2, "u", 1, "d"),
    "lha": (2, "s", 0, "d"), "lhau": (2, "s", 1, "d"), "lwz": (4, "u", 0, "d"), "lwzu": (4, "u", 1, "d"),
    "lwa": (4, "s", 0, "ds"), "ld": (8, "u", 0, "ds"), "ldu": (8, "u", 1, "ds"),
    "lfs": (4, "f", 0, "d"), "lfsu": (4, "f", 1, "d"), "lfd": (8, "f", 0, "d"), "lfdu": (8, "f", 1, "d"),
    "lbzx": (1, "u", 0, "x"), "lbzux": (1, "u", 1, "x"), "lhzx": (2, "u", 0, "x"), "lhzux": (2, "u", 1, "x"),
    "lhax": (2, "s", 0, "x"), "lhaux": (2, "s", 1, "x"), "lwzx": (4, "u", 0, "x"), "lwzux": (4, "u", 1, "x"),
    "lwax": (4, "s", 0, "x"), "lwaux": (4, "s", 1, "x"), "ldx": (8, "u", 0, "x"), "ldux": (8, "u", 1, "x"),
    "lfsx": (4, "f", 0, "x"), "lfsux": (4, "f", 1, "x"), "lfdx": (8, "f", 0, "x"), "lfdux": (8, "f", 1, "x"),
    "lhbrx": (2, "r", 0, "x"), "lwbrx": (4, "r", 0, "x"), "ldbrx": (8, "r", 0, "x"),
}
STORES = {
    "stb": (1, "u", 0, "d"), "stbu": (1, "u", 1, "d"), "sth": (2, "u", 0, "d"), "sthu": (2, "u", 1, "d"),
    "stw": (4, "u", 0, "d"), "stwu": (4, "u", 1, "d"), "std": (8, "u", 0, "ds"), "stdu": (8, "u", 1, "ds"),
    "stfs": (4, "f", 0, "d"), "stfsu": (4, "f", 1, "d"), "stfd": (8, "f", 0, "d"), "stfdu": (8, "f", 1, "d"),
    "stbx": (1, "u", 0, "x"), "stbux": (1, "u", 1, "x"), "sthx": (2, "u", 0, "x"), "sthux": (2, "u", 1, "x"),
    "stwx": (4, "u", 0, "x"), "stwux": (4, "u", 1, "x"), "stdx": (8, "u", 0, "x"), "stdux": (8, "u", 1, "x"),
    "stfsx": (4, "f", 0, "x"), "stfsux": (4, "f", 1, "x"), "stfdx": (8, "f", 0, "x"), "stfdux": (8, "f", 1, "x"),
    "sthbrx": (2, "r", 0, "x"), "stwbrx": (4, "r", 0, "x"), "stdbrx": (8, "r", 0, "x"),
    "stfiwx": (4, "i", 0, "x"),
}

def emit_load(name, w):
    size, kind, upd, form = LOADS[name]
    d, a = f_d(w), f_a(w)
    if form == "x":
        ea = ea_x(a, f_b(w))
    else:
        ea = ea_d(a, f_simm(w) if form == "d" else f_ds(w))
    if kind == "u":
        val = f"ppc_ld{8 * size}(mem_base, ea)"
        dst = R(d)
    elif kind == "s":
        val = f"(uint64_t)(int64_t)(int{8 * size}_t)ppc_ld{8 * size}(mem_base, ea)"
        dst = R(d)
    elif kind == "r":
        val = f"ppc_ld{8 * size}r(mem_base, ea)"
        dst = R(d)
    else:
        val = "(double)ppc_ldf32(mem_base, ea)" if size == 4 else "ppc_ldf64(mem_base, ea)"
        dst = FR(d)
    s = f"{{ uint32_t ea = {ea}; {dst} = {val};"
    if upd:
        s += f" ctx.r[{a}] = ea;"
    return s + " }"

def emit_store(name, w):
    size, kind, upd, form = STORES[name]
    s_, a = f_d(w), f_a(w)
    if form == "x":
        ea = ea_x(a, f_b(w))
    else:
        ea = ea_d(a, f_simm(w) if form == "d" else f_ds(w))
    if kind == "u":
        st = f"ppc_st{8 * size}(mem_base, ea, (uint{8 * size}_t){R(s_)})"
    elif kind == "r":
        st = f"ppc_st{8 * size}r(mem_base, ea, (uint{8 * size}_t){R(s_)})"
    elif kind == "i":
        st = f"ppc_st32(mem_base, ea, (uint32_t)ppc_fbits({FR(s_)}))"
    else:
        st = f"ppc_stf{8 * size}(mem_base, ea, {FR(s_)})"
    s = f"{{ uint32_t ea = {ea}; {st};"
    if upd:
        s += f" ctx.r[{a}] = ea;"
    return s + " }"


def emit_int(name, w):
    """Instruções inteiras/lógicas/rotações/sistema que não dependem do contexto da função.
    Devolve None se não for desta família."""
    d, a, b = f_d(w), f_a(w), f_b(w)
    rc = f_rc(w)
    # ---- D-form aritmética ----
    if name == "addi":
        return f"{R(d)} = (uint64_t){f_simm(w)}ll;" if a == 0 else f"{R(d)} = {R(a)} + (uint64_t){f_simm(w)}ll;"
    if name == "addis":
        v = f_simm(w) << 16
        return f"{R(d)} = (uint64_t){v}ll;" if a == 0 else f"{R(d)} = {R(a)} + (uint64_t){v}ll;"
    if name in ("addic", "addic."):
        s = f"{R(d)} = ppc_add_ca({R(a)}, (uint64_t){f_simm(w)}ll, 0, ctx.xer.ca);"
        return s + (cr0(d) if name == "addic." else "")
    if name == "subfic":
        return f"{R(d)} = ppc_add_ca(~{R(a)}, (uint64_t){f_simm(w)}ll, 1, ctx.xer.ca);"
    if name == "mulli":
        return f"{R(d)} = {R(a)} * (uint64_t){f_simm(w)}ll;"
    if name == "cmpi":
        bf, l = d >> 2, d & 1
        if l:
            return f"ppc_cmp_s(ctx.cr[{bf}], (int64_t){R(a)}, {f_simm(w)}ll, ctx.xer);"
        return f"ppc_cmp_s(ctx.cr[{bf}], (int32_t){R(a)}, {f_simm(w)}, ctx.xer);"
    if name == "cmpli":
        bf, l = d >> 2, d & 1
        if l:
            return f"ppc_cmp_u(ctx.cr[{bf}], {R(a)}, {f_uimm(w)}ull, ctx.xer);"
        return f"ppc_cmp_u(ctx.cr[{bf}], (uint32_t){R(a)}, {f_uimm(w)}u, ctx.xer);"
    if name in ("cmp", "cmpl"):
        bf, l = d >> 2, d & 1
        if name == "cmp":
            t = "int64_t" if l else "int32_t"
            return f"ppc_cmp_s(ctx.cr[{bf}], ({t}){R(a)}, ({t}){R(b)}, ctx.xer);"
        t = "uint64_t" if l else "uint32_t"
        return f"ppc_cmp_u(ctx.cr[{bf}], ({t}){R(a)}, ({t}){R(b)}, ctx.xer);"
    # ---- D-form lógica (rS em d, rA destino) ----
    if name == "ori":
        if d == 0 and a == 0 and f_uimm(w) == 0:
            return ";  // nop"
        return f"{R(a)} = {R(d)} | {f_uimm(w)}ull;"
    if name == "oris":
        return f"{R(a)} = {R(d)} | {hx(f_uimm(w) << 16)};"
    if name == "xori":
        return f"{R(a)} = {R(d)} ^ {f_uimm(w)}ull;"
    if name == "xoris":
        return f"{R(a)} = {R(d)} ^ {hx(f_uimm(w) << 16)};"
    if name == "andi.":
        return f"{R(a)} = {R(d)} & {f_uimm(w)}ull;" + cr0(a)
    if name == "andis.":
        return f"{R(a)} = {R(d)} & {hx(f_uimm(w) << 16)};" + cr0(a)
    # ---- rotações 32 ----
    if name in ("rlwinm", "rlwimi", "rlwnm"):
        sh, mb, me = b, f_c(w), (w >> 1) & 31
        m = mask64(mb + 32, me + 32)
        if name == "rlwnm":
            rot = f"ppc_rotl32d({R(d)}, (unsigned){R(b)})"
        elif mb <= me:
            rot = f"(uint64_t)std::rotl((uint32_t){R(d)}, {sh})" if sh else f"(uint64_t)(uint32_t){R(d)}"
            m &= 0xFFFFFFFF
        else:
            rot = f"ppc_rotl32d({R(d)}, {sh})"
        if name == "rlwimi":
            s = f"{R(a)} = ({rot} & {hx(m)}) | ({R(a)} & {hx(~m)});"
        else:
            s = f"{R(a)} = {rot} & {hx(m)};"
        return s + (cr0(a) if rc else "")
    # ---- rotações 64 ----
    if name in ("rldicl", "rldicr", "rldic", "rldimi"):
        sh = b | (((w >> 1) & 1) << 5)
        mbx = ((w >> 6) & 31) | (((w >> 5) & 1) << 5)
        rot = f"std::rotl({R(d)}, {sh})" if sh else R(d)
        if name == "rldicl":
            m = mask64(mbx, 63)
        elif name == "rldicr":
            m = mask64(0, mbx)
        else:
            m = mask64(mbx, 63 - sh)
        if name == "rldimi":
            s = f"{R(a)} = ({rot} & {hx(m)}) | ({R(a)} & {hx(~m)});"
        else:
            s = f"{R(a)} = {rot} & {hx(m)};"
        return s + (cr0(a) if rc else "")
    if name in ("rldcl", "rldcr"):
        mbx = ((w >> 6) & 31) | (((w >> 5) & 1) << 5)
        m = mask64(mbx, 63) if name == "rldcl" else mask64(0, mbx)
        return f"{R(a)} = std::rotl({R(d)}, (int)({R(b)} & 63)) & {hx(m)};" + (cr0(a) if rc else "")
    # ---- XO-form aritmética ----
    xo = {
        "add": f"{R(d)} = {R(a)} + {R(b)};",
        "subf": f"{R(d)} = {R(b)} - {R(a)};",
        "neg": f"{R(d)} = 0 - {R(a)};",
        "addc": f"{R(d)} = ppc_add_ca({R(a)}, {R(b)}, 0, ctx.xer.ca);",
        "adde": f"{R(d)} = ppc_add_ca({R(a)}, {R(b)}, ctx.xer.ca, ctx.xer.ca);",
        "addze": f"{R(d)} = ppc_add_ca({R(a)}, 0, ctx.xer.ca, ctx.xer.ca);",
        "addme": f"{R(d)} = ppc_add_ca({R(a)}, ~0ull, ctx.xer.ca, ctx.xer.ca);",
        "subfc": f"{R(d)} = ppc_add_ca(~{R(a)}, {R(b)}, 1, ctx.xer.ca);",
        "subfe": f"{R(d)} = ppc_add_ca(~{R(a)}, {R(b)}, ctx.xer.ca, ctx.xer.ca);",
        "subfze": f"{R(d)} = ppc_add_ca(~{R(a)}, 0, ctx.xer.ca, ctx.xer.ca);",
        "subfme": f"{R(d)} = ppc_add_ca(~{R(a)}, ~0ull, ctx.xer.ca, ctx.xer.ca);",
        "mullw": f"{R(d)} = (uint64_t)((int64_t)(int32_t){R(a)} * (int64_t)(int32_t){R(b)});",
        "mulld": f"{R(d)} = {R(a)} * {R(b)};",
        "mulhw": f"{R(d)} = ppc_mulhw({R(a)}, {R(b)});",
        "mulhwu": f"{R(d)} = ppc_mulhwu({R(a)}, {R(b)});",
        "mulhd": f"{R(d)} = ppc_mulhd({R(a)}, {R(b)});",
        "mulhdu": f"{R(d)} = ppc_mulhdu({R(a)}, {R(b)});",
        "divw": f"{R(d)} = ppc_divw({R(a)}, {R(b)});",
        "divwu": f"{R(d)} = ppc_divwu({R(a)}, {R(b)});",
        "divd": f"{R(d)} = ppc_divd({R(a)}, {R(b)});",
        "divdu": f"{R(d)} = ppc_divdu({R(a)}, {R(b)});",
    }.get(name)
    if xo:
        # OE (bit 10) não é modelado: XER[OV/SO] só mudam via mtxer.
        return xo + (cr0(d) if rc else "")
    # ---- X-form lógica (rS=d, rA destino) ----
    s = d
    xl = {
        "and": f"{R(a)} = {R(s)} & {R(b)};",
        "andc": f"{R(a)} = {R(s)} & ~{R(b)};",
        "or": f"{R(a)} = {R(s)} | {R(b)};" if s != b else f"{R(a)} = {R(s)};",
        "orc": f"{R(a)} = {R(s)} | ~{R(b)};",
        "xor": f"{R(a)} = {R(s)} ^ {R(b)};",
        "nor": f"{R(a)} = ~({R(s)} | {R(b)});",
        "nand": f"{R(a)} = ~({R(s)} & {R(b)});",
        "eqv": f"{R(a)} = ~({R(s)} ^ {R(b)});",
        "slw": f"{R(a)} = ppc_slw({R(s)}, {R(b)});",
        "srw": f"{R(a)} = ppc_srw({R(s)}, {R(b)});",
        "sld": f"{R(a)} = ppc_sld({R(s)}, {R(b)});",
        "srd": f"{R(a)} = ppc_srd({R(s)}, {R(b)});",
        "sraw": f"{R(a)} = ppc_sraw({R(s)}, (unsigned){R(b)}, ctx.xer.ca);",
        "srad": f"{R(a)} = ppc_srad({R(s)}, (unsigned){R(b)}, ctx.xer.ca);",
        "srawi": f"{R(a)} = ppc_sraw({R(s)}, {b}, ctx.xer.ca);",
        "sradi": f"{R(a)} = ppc_srad({R(s)}, {b | (((w >> 1) & 1) << 5)}, ctx.xer.ca);",
        "cntlzw": f"{R(a)} = (uint64_t)std::countl_zero((uint32_t){R(s)});",
        "cntlzd": f"{R(a)} = (uint64_t)std::countl_zero({R(s)});",
        "extsb": f"{R(a)} = (uint64_t)(int64_t)(int8_t){R(s)};",
        "extsh": f"{R(a)} = (uint64_t)(int64_t)(int16_t){R(s)};",
        "extsw": f"{R(a)} = (uint64_t)(int64_t)(int32_t){R(s)};",
    }.get(name)
    if xl:
        return xl + (cr0(a) if rc else "")
    # ---- loads/stores ----
    if name in LOADS:
        return emit_load(name, w)
    if name in STORES:
        return emit_store(name, w)
    if name == "lmw":
        return (f"{{ uint32_t ea = {ea_d(a, f_simm(w))}; for (int i = {d}; i < 32; i++, ea += 4) "
                f"ctx.r[i] = ppc_ld32(mem_base, ea); }}")
    if name == "stmw":
        return (f"{{ uint32_t ea = {ea_d(a, f_simm(w))}; for (int i = {d}; i < 32; i++, ea += 4) "
                f"ppc_st32(mem_base, ea, (uint32_t)ctx.r[i]); }}")
    if name == "lswi":
        nb = b or 32
        out = [f"{{ uint32_t ea = {'0u' if a == 0 else f'(uint32_t){R(a)}'};"]
        r, i = d, 0
        while i < nb:
            out.append(f" {R(r)} = 0;")
            for k in range(4):
                if i < nb:
                    out.append(f" {R(r)} |= (uint64_t)ppc_ld8(mem_base, ea + {i}) << {24 - 8 * k};")
                    i += 1
            r = (r + 1) & 31
        return "".join(out) + " }"
    if name == "stswi":
        nb = b or 32
        out = [f"{{ uint32_t ea = {'0u' if a == 0 else f'(uint32_t){R(a)}'};"]
        r, i = d, 0
        while i < nb:
            for k in range(4):
                if i < nb:
                    out.append(f" ppc_st8(mem_base, ea + {i}, (uint8_t)({R(r)} >> {24 - 8 * k}));")
                    i += 1
            r = (r + 1) & 31
        return "".join(out) + " }"
    if name == "lwarx":
        return f"{R(d)} = ppc_lwarx(ctx, mem_base, {ea_x(a, b)});"
    if name == "ldarx":
        return f"{R(d)} = ppc_ldarx(ctx, mem_base, {ea_x(a, b)});"
    if name == "stwcx.":
        return (f"{{ bool ok = ppc_stwcx(ctx, mem_base, {ea_x(a, b)}, (uint32_t){R(d)}); "
                f"ctx.cr[0].lt = 0; ctx.cr[0].gt = 0; ctx.cr[0].eq = ok; ctx.cr[0].so = ctx.xer.so; }}")
    if name == "stdcx.":
        return (f"{{ bool ok = ppc_stdcx(ctx, mem_base, {ea_x(a, b)}, {R(d)}); "
                f"ctx.cr[0].lt = 0; ctx.cr[0].gt = 0; ctx.cr[0].eq = ok; ctx.cr[0].so = ctx.xer.so; }}")
    if name == "dcbz":
        size = 128 if (w >> 21) & 1 else 32
        return f"ppc_dcbz(mem_base, {ea_x(a, b)}, {size});"
    if name in ("dcbt", "dcbtst", "dcbf", "dcbst", "dcbi", "icbi", "isync"):
        return ";"
    if name in ("sync", "eieio"):
        return "std::atomic_thread_fence(std::memory_order_seq_cst);"
    # ---- registos especiais ----
    if name in ("mfspr", "mtspr", "mftb"):
        spr = ((w >> 16) & 31) | (((w >> 11) & 31) << 5)
        if name == "mftb" or (name == "mfspr" and spr in (268, 269)):
            return f"{R(d)} = ppc_mftb();" if spr == 268 else f"{R(d)} = ppc_mftb() >> 32;"
        if name == "mfspr":
            src = {1: "(uint64_t)ppc_get_xer(ctx)", 8: "ctx.lr", 9: "ctx.ctr", 256: "(uint64_t)ctx.vrsave"}.get(spr)
            return f"{R(d)} = {src};" if src else None
        dst = {1: f"ppc_set_xer(ctx, {R(d)});", 8: f"ctx.lr = {R(d)};", 9: f"ctx.ctr = {R(d)};",
               256: f"ctx.vrsave = (uint32_t){R(d)};"}.get(spr)
        return dst
    if name == "mfcr":
        return f"{R(d)} = ppc_get_cr(ctx);"
    if name == "mtcrf":
        return f"ppc_set_cr(ctx, (uint32_t){R(d)}, {(w >> 12) & 0xFF});"
    if name == "mfmsr":
        return f"{R(d)} = ctx.msr;"
    if name in ("mtmsr", "mtmsrd"):
        return f"ctx.msr = {R(d)};"
    # ---- CR lógico ----
    if name == "mcrf":
        return f"ctx.cr[{d >> 2}] = ctx.cr[{a >> 2}];"
    crops = {"crand": "{x} & {y}", "cror": "{x} | {y}", "crxor": "{x} ^ {y}", "crnand": "!({x} & {y})",
             "crnor": "!({x} | {y})", "creqv": "!({x} ^ {y})", "crandc": "{x} & !{y}", "crorc": "{x} | !{y}"}
    if name in crops:
        bit = lambda n: f"ctx.cr[{n >> 2}].{CRB[n & 3]}"
        return f"{bit(d)} = (uint8_t)({crops[name].format(x=bit(a), y=bit(b))});"
    # ---- traps / sistema ----
    if name in ("tw", "twi", "td", "tdi"):
        to = d
        if name in ("twi", "tdi"):
            rhs = f"{f_simm(w)}ll"
        else:
            rhs = f"(int64_t){R(b)}" if name == "td" else f"(int64_t)(int32_t){R(b)}"
        lhs = f"(int64_t){R(a)}" if name in ("td", "tdi") else f"(int64_t)(int32_t){R(a)}"
        if to == 31:
            return f"ppc_trap(ctx, mem_base, {{ADDR}}, 0x{w:08X}u);"
        return f"if (ppc_trap_cond({to}, {lhs}, {rhs})) ppc_trap(ctx, mem_base, {{ADDR}}, 0x{w:08X}u);"
    if name == "sc":
        return "ppc_syscall(ctx, mem_base, {ADDR});"
    return None


def emit_fp(name, w):
    d, a, b, c = f_d(w), f_a(w), f_b(w), f_c(w)
    rc1 = " ppc_fp_cr1(ctx);" if f_rc(w) else ""
    A, B, C, D = FR(a), FR(b), FR(c), FR(d)
    ops = {
        "fadd": f"{A} + {B}", "fsub": f"{A} - {B}", "fmul": f"{A} * {C}", "fdiv": f"{A} / {B}",
        "fsqrt": f"std::sqrt({B})", "fsel": f"ppc_fsel({A}, {C}, {B})", "frsqrte": f"1.0 / std::sqrt({B})",
        "fmadd": f"std::fma({A}, {C}, {B})", "fmsub": f"std::fma({A}, {C}, -{B})",
        "fnmadd": f"-std::fma({A}, {C}, {B})", "fnmsub": f"-std::fma({A}, {C}, -{B})",
        "frsp": f"ppc_f2s({B})", "fctiw": f"ppc_fctiw({B}, false)", "fctiwz": f"ppc_fctiw({B}, true)",
        "fctid": f"ppc_fctid({B}, false)", "fctidz": f"ppc_fctid({B}, true)", "fcfid": f"ppc_fcfid({B})",
        "fneg": f"-{B}", "fmr": B, "fabs": f"std::fabs({B})", "fnabs": f"ppc_fnabs({B})",
        "mffs": "ppc_fbits_d((uint64_t)ctx.fpscr)",
    }
    sops = {
        "fadds": f"{A} + {B}", "fsubs": f"{A} - {B}", "fmuls": f"{A} * {C}", "fdivs": f"{A} / {B}",
        "fsqrts": f"std::sqrt({B})", "fres": f"(double)(1.0f / (float){B})",
        "fmadds": f"std::fma({A}, {C}, {B})", "fmsubs": f"std::fma({A}, {C}, -{B})",
        "fnmadds": f"-std::fma({A}, {C}, {B})", "fnmsubs": f"-std::fma({A}, {C}, -{B})",
    }
    if name in ops:
        return f"{D} = {ops[name]};{rc1}"
    if name in sops:
        return f"{D} = ppc_f2s({sops[name]});{rc1}"
    if name in ("fcmpu", "fcmpo"):
        return f"ppc_cmp_f(ctx.cr[{d >> 2}], {A}, {B});"
    if name == "mtfsf":
        return f"ppc_mtfsf(ctx, {(w >> 17) & 0xFF}, {B});{rc1}"
    if name == "mtfsfi":
        sh = 28 - 4 * (d >> 2)
        imm = (w >> 12) & 0xF
        return f"ppc_set_fpscr(ctx, (ctx.fpscr & ~(0xFu << {sh})) | ({imm}u << {sh}));{rc1}"
    if name in ("mtfsb0", "mtfsb1"):
        bit = 1 << (31 - d)
        if name == "mtfsb0":
            return f"ppc_set_fpscr(ctx, ctx.fpscr & ~0x{bit:X}u);{rc1}"
        return f"ppc_set_fpscr(ctx, ctx.fpscr | 0x{bit:X}u);{rc1}"
    if name == "mcrfs":
        return f"ppc_cr_set4(ctx.cr[{d >> 2}], (ctx.fpscr >> {28 - 4 * (a >> 2)}) & 0xF);"
    return None


# Vetores
VEC_AB = {  # vD = op(vA, vB)
    "vaddubm", "vadduhm", "vadduwm", "vaddubs", "vadduhs", "vadduws", "vaddsbs", "vaddshs", "vaddsws",
    "vaddcuw", "vsububm", "vsubuhm", "vsubuwm", "vsububs", "vsubuhs", "vsubuws", "vsubsbs", "vsubshs",
    "vsubsws", "vsubcuw", "vmaxub", "vmaxuh", "vmaxuw", "vmaxsb", "vmaxsh", "vmaxsw", "vminub", "vminuh",
    "vminuw", "vminsb", "vminsh", "vminsw", "vavgub", "vavguh", "vavguw", "vavgsb", "vavgsh", "vavgsw",
    "vand", "vandc", "vor", "vnor", "vxor", "vslb", "vslh", "vslw", "vsrb", "vsrh", "vsrw", "vsrab",
    "vsrah", "vsraw", "vrlb", "vrlh", "vrlw", "vaddfp", "vsubfp", "vmaxfp", "vminfp", "vmrghb", "vmrghh",
    "vmrghw", "vmrglb", "vmrglh", "vmrglw", "vpkuhum", "vpkuwum", "vpkuhus", "vpkuwus", "vpkshss",
    "vpkshus", "vpkswss", "vpkswus", "vpkpx", "vslo", "vsro", "vsl", "vsr", "vmuleub", "vmuloub",
    "vmulesb", "vmulosb", "vmuleuh", "vmulouh", "vmulesh", "vmulosh", "vsumsws", "vsum2sws",
    "vsum4ubs", "vsum4sbs", "vsum4shs",
}
VEC_B = {"vrefp", "vrsqrtefp", "vexptefp", "vlogefp", "vrfin", "vrfiz", "vrfip", "vrfim",
         "vupkhsb", "vupklsb", "vupkhsh", "vupklsh"}
VEC_CMP = {"vcmpequb", "vcmpequh", "vcmpequw", "vcmpeqfp", "vcmpgefp", "vcmpgtub", "vcmpgtuh",
           "vcmpgtuw", "vcmpgtfp", "vcmpgtsb", "vcmpgtsh", "vcmpgtsw", "vcmpbfp"}
VEC_LS = {  # nome: (helper, é_store)
    "lvx": ("ppc_lvx", 0), "lvxl": ("ppc_lvx", 0), "lvebx": ("ppc_lvebx", 0), "lvehx": ("ppc_lvehx", 0),
    "lvewx": ("ppc_lvewx", 0), "lvlx": ("ppc_lvlx", 0), "lvlxl": ("ppc_lvlx", 0), "lvrx": ("ppc_lvrx", 0),
    "lvrxl": ("ppc_lvrx", 0), "stvx": ("ppc_stvx", 1), "stvxl": ("ppc_stvx", 1),
    "stvebx": ("ppc_stvebx", 1), "stvehx": ("ppc_stvehx", 1), "stvewx": ("ppc_stvewx", 1),
    "stvlx": ("ppc_stvlx", 1), "stvlxl": ("ppc_stvlx", 1), "stvrx": ("ppc_stvrx", 1),
    "stvrxl": ("ppc_stvrx", 1),
}


def emit_vec(name, w):
    op = w >> 26
    if name.endswith("128"):
        base = name[:-3]
    elif name.endswith("128."):
        base = name[:-4] + "."
    else:
        base = name
    is128 = name != base
    # ---- loads/stores (opcode 31 e VX128_1) ----
    if base in VEC_LS or base in ("lvsl", "lvsr"):
        vd = v128_d(w) if is128 else f_d(w)
        ea = ea_x(f_a(w), f_b(w))
        if base == "lvsl":
            return f"ppc_lvsl({VR(vd)}, {ea});"
        if base == "lvsr":
            return f"ppc_lvsr({VR(vd)}, {ea});"
        fn, _st = VEC_LS[base]
        return f"{fn}({VR(vd)}, mem_base, {ea});"
    if is128:
        vd, va, vb = v128_d(w), v128_a(w), v128_b(w)
    else:
        vd, va, vb = f_d(w), f_a(w), f_b(w)
    vc = f_c(w)
    D, A, B = VR(vd), VR(va), VR(vb)
    rc = False
    if base.endswith("."):
        base, rc = base[:-1], True
    if base in VEC_CMP:
        s = f"ppc_{base}({D}, {A}, {B});"
        if not is128:
            rc = bool(w & 0x400)
        if rc:
            s += f" ppc_vcmp{'b' if base == 'vcmpbfp' else ''}_cr6(ctx.cr[6], {D});"
        return s
    if base in VEC_AB or (is128 and base in ("vmulfp",)):
        return f"ppc_{base}({D}, {A}, {B});"
    if base in VEC_B:
        return f"ppc_{base}({D}, {B});"
    if not is128:
        if base in ("vspltb", "vsplth", "vspltw"):
            return f"ppc_{base}({D}, {B}, {va});"
        if base in ("vspltisb", "vspltish", "vspltisw"):
            return f"ppc_{base}({D}, {sext(va, 5)});"
        if base in ("vcfsx", "vcfux", "vctsxs", "vctuxs"):
            return f"ppc_{base}({D}, {B}, {va});"
        if base in ("vupkhpx", "vupklpx"):
            return f"ppc_vupkpx({D}, {B}, {0 if base == 'vupkhpx' else 4});"
        if base == "mfvscr":
            return f"{D} = PPCVReg{{}}; PPC_VW32({D}, 3) = ctx.vscr;"
        if base == "mtvscr":
            return f"ctx.vscr = PPC_VW32({B}, 3);"
        C = VR(vc)
        if base in ("vperm", "vsel"):
            return f"ppc_{base}({D}, {A}, {B}, {C});"
        if base == "vsldoi":
            return f"ppc_vsldoi({D}, {A}, {B}, {(w >> 6) & 15});"
        if base == "vmaddfp":
            return f"ppc_vmaddfp({D}, {A}, {C}, {B});"
        if base == "vnmsubfp":
            return f"ppc_vnmsubfp({D}, {A}, {C}, {B});"
        if base in ("vmsumshm", "vmsumuhm", "vmsumubm", "vmladduhm", "vmhaddshs", "vmhraddshs"):
            return f"ppc_{base}({D}, {A}, {B}, {C});"
        return None
    # ---- VMX128 ----
    imm = (w >> 16) & 31
    if base == "vperm":
        return f"ppc_vperm({D}, {A}, {B}, {VR((w >> 6) & 7)});"
    if base == "vsldoi":
        return f"ppc_vsldoi({D}, {A}, {B}, {(w >> 6) & 15});"
    if base == "vmaddfp":
        return f"ppc_vmaddfp({D}, {A}, {B}, {D});"
    if base == "vmaddcfp":
        return f"ppc_vmaddfp({D}, {A}, {D}, {B});"
    if base == "vnmsubfp":
        return f"ppc_vnmsubfp({D}, {A}, {B}, {D});"
    if base in ("vmsum3fp", "vmsum4fp"):
        return f"ppc_{base}({D}, {A}, {B});"
    if base == "vsel":
        return f"ppc_vsel({D}, {A}, {B}, {D});"
    if base == "vcfpsxws":
        return f"ppc_vctsxs({D}, {B}, {imm});"
    if base == "vcfpuxws":
        return f"ppc_vctuxs({D}, {B}, {imm});"
    if base == "vcsxwfp":
        return f"ppc_vcfsx({D}, {B}, {imm});"
    if base == "vcuxwfp":
        return f"ppc_vcfux({D}, {B}, {imm});"
    if base == "vspltw":
        return f"ppc_vspltw({D}, {B}, {imm});"
    if base == "vspltisw":
        return f"ppc_vspltisw({D}, {sext(imm, 5)});"
    if base == "vpermwi":
        perm = imm | (((w >> 6) & 7) << 5)
        return f"ppc_vpermwi({D}, {B}, {perm});"
    if base == "vrlimi":
        return f"ppc_vrlimi({D}, {B}, {imm}, {(w >> 6) & 3});"
    if base == "vupkd3d":
        return f"ppc_vupkd3d({D}, {B}, {imm >> 2}, ctx, mem_base, {{ADDR}}, 0x{w:08X}u);"
    if base == "vpkd3d":
        return f"ppc_vpkd3d({D}, {B}, {imm >> 2}, {imm & 3}, {(w >> 6) & 3}, ctx, mem_base, {{ADDR}}, 0x{w:08X}u);"
    return None


# =============================================================================================
# Simulação concreta para tabelas de salto (switch)
# =============================================================================================
NO_GPR_WRITE_PREFIX = ("st", "cmp", "b", "dcb", "icb", "sync", "eieio", "isync", "tw", "td",
                       "cr", "mcrf", "mtcrf", "mtfs", "mtmsr", "f", "lf", "lv", "v")


class JumpTableSim:
    def __init__(self, img):
        self.img = img

    def run(self, seq, idx_reg, idx_pos, i):
        """Simula `seq` (lista de (addr, w, name)) com o registo de índice = i a partir de idx_pos.
        Devolve o valor de CTR ou None."""
        regs = {}
        stack = {}  # slots relativos a r1 (código sem otimização recarrega o índice da pilha)
        alias = {}  # registo -> slot da pilha com o mesmo valor
        ctr = None
        img = self.img
        for k, (addr, w, name) in enumerate(seq):
            if k == idx_pos:
                regs[idx_reg] = i
                if idx_reg in alias:
                    stack[alias[idx_reg]] = i
            d, a, b = f_d(w), f_a(w), f_b(w)
            g = regs.get
            if a == 1 and name in ("stw", "lwz"):
                off = f_simm(w)
                if name == "stw":
                    stack[off] = g(d)
                    alias[d] = off
                else:
                    regs[d] = stack.get(off)
                    alias[d] = off
                continue
            if name not in ("cmpli", "cmpi", "cmp", "cmpl", "mtspr") and not name.startswith(("st", "b")):
                alias.pop(d, None)
                alias.pop(a, None)
            if name in ("addi", "addis"):
                imm = f_simm(w) << (16 if name == "addis" else 0)
                base = 0 if a == 0 else g(a)
                regs[d] = None if base is None else (base + imm) & 0xFFFFFFFF
            elif name in ("ori", "oris"):
                v = g(d)
                regs[a] = None if v is None else v | (f_uimm(w) << (16 if name == "oris" else 0))
            elif name == "rlwinm":
                v = g(d)
                if v is None:
                    regs[a] = None
                else:
                    sh, mb, me = b, f_c(w), (w >> 1) & 31
                    v &= 0xFFFFFFFF
                    rot = ((v << sh) | (v >> (32 - sh))) & 0xFFFFFFFF if sh else v
                    regs[a] = rot & (mask64(mb + 32, me + 32) & 0xFFFFFFFF)
            elif name == "add":
                x, y = g(a), g(b)
                regs[d] = None if x is None or y is None else (x + y) & 0xFFFFFFFF
            elif name == "subf":
                x, y = g(a), g(b)
                regs[d] = None if x is None or y is None else (y - x) & 0xFFFFFFFF
            elif name == "or":
                x, y = g(d), g(b)
                regs[a] = None if x is None or y is None else x | y
            elif name in ("extsb", "extsh"):
                v = g(d)
                bits = 8 if name == "extsb" else 16
                regs[a] = None if v is None else sext(v & ((1 << bits) - 1), bits) & 0xFFFFFFFF
            elif name in ("lbzx", "lhzx", "lwzx", "lhax", "lbz", "lhz", "lwz", "lha"):
                if name.endswith("x"):
                    x = 0 if a == 0 else g(a)
                    y = g(b)
                    ea = None if x is None or y is None else (x + y) & 0xFFFFFFFF
                else:
                    x = 0 if a == 0 else g(a)
                    ea = None if x is None else (x + f_simm(w)) & 0xFFFFFFFF
                size = {"b": 1, "h": 2, "w": 4}[name[1]]
                v = img.read(ea, size) if ea is not None else None
                if v is not None and name.startswith("lha"):
                    v = sext(v, 16) & 0xFFFFFFFF
                regs[d] = v
            elif name == "mtspr":
                spr = ((w >> 16) & 31) | (((w >> 11) & 31) << 5)
                if spr == 9:
                    ctr = g(d)
            elif name.startswith(NO_GPR_WRITE_PREFIX) and not name.startswith(("lf",)):
                pass
            else:
                regs[d] = None
                regs[a] = None
        return ctr


# =============================================================================================
# Imagem / análise
# =============================================================================================
class Image:
    def __init__(self, path, base):
        with open(path, "rb") as fp:
            self.data = fp.read()
        self.base = base
        self.end = base + len(self.data)

    def word(self, addr):
        return struct.unpack_from(">I", self.data, addr - self.base)[0]

    def read(self, addr, size):
        if addr is None or addr < self.base or addr + size > self.end:
            return None
        return int.from_bytes(self.data[addr - self.base: addr - self.base + size], "big")


def branch_target(addr, w, name):
    if name == "b":
        li = sext(w & 0x03FFFFFC, 26)
        return (li if w & 2 else addr + li) & 0xFFFFFFFF
    if name == "bc":
        bd = sext(w & 0xFFFC, 16)
        return (bd if w & 2 else addr + bd) & 0xFFFFFFFF
    return None


class Recompiler:
    def __init__(self, args):
        self.args = args
        with open(args.map, encoding="utf-8") as fp:
            self.map = json.load(fp)
        with open(args.pdata, encoding="utf-8") as fp:
            self.pdata = json.load(fp)
        base = int(self.map["image_base_address"], 16)
        self.img = Image(args.image, base)
        self.entry_point = int(self.map["entry_point"], 16)
        self.code_ranges = []
        for name, sec in self.map["sections"].items():
            if "MEM_EXECUTE" in sec["flags"]:  # .text, .embsec_*, BINK
                self.code_ranges.append((int(sec["virtual_address"], 16), int(sec["end"], 16)))
        self.code_ranges.sort()
        self.imports = {}  # thunk -> (lib_index, ordinal, lib_name)
        self.import_vars = []
        for lib in self.map["imports"]:
            for imp in lib["imports"]:
                if imp["type"] == "function" and imp.get("thunk_address"):
                    self.imports[int(imp["thunk_address"], 16)] = (lib["library_index"], imp["ordinal"], lib["name"])
                elif imp["type"] == "variable":
                    self.import_vars.append((lib["name"], imp["ordinal"], int(imp["variable_address"], 16)))
        self.decode_cache = {}
        self.stats = collections.Counter()
        self.unimpl = collections.Counter()
        self.unimpl_sites = collections.defaultdict(list)
        self.unknown_words = collections.Counter()
        self.warnings = []
        self.jt_ok = 0
        self.jt_unbounded = 0
        self.jt_fail = []
        self.unresolved_branches = []

    # ------------------------------------------------------------------
    def in_code(self, addr):
        for s, e in self.code_ranges:
            if s <= addr < e:
                return True
        return False

    def code_end(self, addr):
        for s, e in self.code_ranges:
            if s <= addr < e:
                return e
        return addr

    def insn(self, addr):
        c = self.decode_cache.get(addr)
        if c is None:
            w = self.img.word(addr)
            c = (w, decode(w))
            self.decode_cache[addr] = c
        return c

    # ------------------------------------------------------------------
    def build_cfg(self):
        """Delimita as rotinas: .pdata + alvos de bl + alvos de saltos entre rotinas."""
        regions = {}
        for f in self.pdata:
            s = int(f["address"], 16)
            regions[s] = s + f["length"]
        self.pdata_starts = set(regions)
        starts = sorted(regions)
        entries = set(regions) | {self.entry_point}
        self.call_targets = set()
        worklist = list(starts)
        scanned = set()
        synthetic = 0

        def drain():
            nonlocal synthetic
            while worklist:
                rs = worklist.pop()
                if rs in scanned:
                    continue
                scanned.add(rs)
                re_ = regions[rs]
                for addr in range(rs, re_, 4):
                    w, name = self.insn(addr)
                    if name not in ("b", "bc"):
                        continue
                    t = branch_target(addr, w, name)
                    lk = w & 1
                    if lk and t == addr + 4:
                        continue  # bl $+4 (obter PC)
                    if not lk and rs <= t < re_:
                        continue  # salto local
                    if t in self.imports:
                        continue
                    if not self.in_code(t) or t & 3:
                        self.warnings.append(f"alvo fora do código: 0x{addr:08X} -> 0x{t:08X}")
                        continue
                    if lk:
                        self.call_targets.add(t)
                    if t in entries:
                        continue
                    entries.add(t)
                    # contido numa região existente?
                    i = bisect.bisect_right(starts, t) - 1
                    if i >= 0 and regions[starts[i]] > t:
                        continue  # ponto de entrada no meio de uma rotina
                    # nova região sintética
                    nxt = starts[i + 1] if i + 1 < len(starts) else self.code_end(t)
                    end = self.refine_end(t, min(nxt, self.code_end(t)))
                    regions[t] = end
                    bisect.insort(starts, t)
                    worklist.append(t)
                    synthetic += 1

        drain()
        # Rotinas sem .pdata alcançadas só por ponteiro (vtables, callbacks, threads): ponteiros para
        # código nas secções de dados e pares lis/addi no código. Só alvos fora de qualquer região
        # viram rotinas; dentro de regiões .pdata são constantes ou escopos SEH (ignorados).
        # Ponto fixo: as regiões novas (ex.: inicializadores estáticos alcançados só pela tabela do CRT, ou as
        # descobertas pelo drain) também montam ponteiros com lis/addi e têm de ser varridas.
        seeded = seeded_mid = 0
        scan, with_data, passes = list(starts), True, 0
        while scan:
            passes += 1
            before = set(regions)
            for t in sorted(self.pointer_seeds(regions, scan, with_data)):
                if t in entries or t in self.imports:
                    continue
                i = bisect.bisect_right(starts, t) - 1
                if i >= 0 and regions[starts[i]] > t:
                    if starts[i] not in self.pdata_starts:
                        entries.add(t)  # ponteiro para o meio de uma região sintética
                        seeded_mid += 1
                    continue
                nxt = starts[i + 1] if i + 1 < len(starts) else self.code_end(t)
                end = self.refine_end(t, min(nxt, self.code_end(t)))
                if end <= t:
                    continue
                entries.add(t)
                regions[t] = end
                bisect.insort(starts, t)
                worklist.append(t)
                seeded += 1
            drain()
            scan, with_data = sorted(set(regions) - before), False
        self.stats["pointer_seed_passes"] = passes
        # Entradas internas: saltos para trás do início da entrada (laços) precisam de um alvo
        # chamável, senão viram ppc_call_indirect para um endereço sem rotina.
        pending = [e for e in entries if e not in regions]
        while pending:
            e = pending.pop()
            i = bisect.bisect_right(starts, e) - 1
            rs, re_ = starts[i], regions[starts[i]]
            for addr in range(e, re_, 4):
                w, name = self.insn(addr)
                if name in ("b", "bc") and not (w & 1):
                    t = branch_target(addr, w, name)
                    if rs <= t < e and t not in entries:
                        entries.add(t)
                        pending.append(t)
        self.stats["regions_pointer_seeded"] = seeded
        self.stats["entries_pointer_seeded_mid"] = seeded_mid
        # Rotinas do .pdata cujo switch salta para depois do fim declarado: estende até aos casos
        rstarts = sorted(regions)
        extended = 0
        for i, rs in enumerate(rstarts):
            re_ = regions[rs]
            limit = rstarts[i + 1] if i + 1 < len(rstarts) else self.code_end(rs)
            body = [(x,) + self.insn(x) for x in range(rs, re_, 4)]
            for k, (addr, w, name) in enumerate(body):
                if name == "bcctr" and not (w & 1):
                    tbl = self.find_jump_table(body, k, rs, limit, count=False)
                    if tbl and max(tbl) >= re_:
                        regions[rs] = self.refine_end(max(tbl), limit)
                        extended += 1
                        break
        self.stats["regions_extended_for_switch"] = extended
        self.regions = regions
        self.region_starts = sorted(regions)
        self.entries = sorted(entries)
        self.funcs = set(entries) | set(self.imports)
        self.stats["regions_pdata"] = len(self.pdata_starts)
        self.stats["regions_synthetic"] = synthetic
        self.stats["entries"] = len(self.entries)
        self.stats["entries_mid_region"] = sum(1 for e in self.entries if e not in regions)
        self.stats["import_thunks"] = len(self.imports)

    def pointer_seeds(self, regions, starts, with_data=True):
        """Candidatos a início de rotina vindos de ponteiros: palavras BE das secções de dados que
        apontam para código (se `with_data`) e endereços montados por `lis rX,hi` + `addi/ori rY,rX,lo`
        nas regiões `starts`."""
        def plausible(t):
            if t & 3 or not self.in_code(t):
                return False
            w, name = self.insn(t)
            # rejeita padding e tabelas de salto (entradas são elas próprias ponteiros para código)
            return name is not None and w != 0 and not self.in_code(w)

        seeds = set()
        for name, sec in (self.map["sections"].items() if with_data else ()):
            if "MEM_EXECUTE" in sec["flags"] or name in (".pdata", ".reloc", ".idata", ".XBLD"):
                continue
            s, e = int(sec["virtual_address"], 16), int(sec["end"], 16)
            for addr in range(s, min(e, self.img.end) & ~3, 4):
                t = self.img.word(addr)
                if plausible(t):
                    seeds.add(t)
        for rs in starts:
            for addr in range(rs, regions[rs], 4):
                w, name = self.insn(addr)
                if name != "addis" or f_a(w) != 0:
                    continue
                rx, hi = f_d(w), (f_uimm(w) << 16) & 0xFFFFFFFF
                # O compilador agenda vários `lis` bem antes dos `addi` e reutiliza o mesmo `lis` para vários
                # endereços: segue todos os usos de rX em até 64 instruções, até rX ser redefinido.
                for a2 in range(addr + 4, min(addr + 256, regions[rs]), 4):
                    w2, n2 = self.insn(a2)
                    if n2 in ("addi", "ori") and f_a(w2) == rx:
                        lo = sext(f_uimm(w2), 16) if n2 == "addi" else f_uimm(w2)
                        t = (hi + lo) & 0xFFFFFFFF if n2 == "addi" else hi | lo
                        if plausible(t):
                            seeds.add(t)
                        if f_d(w2) == rx:
                            break
                        continue
                    if n2 is None:
                        break
                    if n2 in ("b", "bclr", "bcctr") and not (w2 & 1) and \
                            (n2 == "b" or ((w2 >> 21) & 0x14) == 0x14):
                        break  # fim do bloco
                    if n2 == "b" and (w2 & 1) and (rx == 0 or 3 <= rx <= 12):
                        break  # chamada: registrador volátil destruído
                    if f_d(w2) == rx and not n2.startswith(("st", "b", "cmp", "tw", "td")):
                        break  # rX redefinido
        return seeds

    def refine_end(self, start, hard_end):
        """Para regiões fora do .pdata: termina após o último terminador incondicional que não
        tenha saltos locais pendentes para a frente."""
        max_target = start
        addr = start
        body = []
        while addr < hard_end:
            w, name = self.insn(addr)
            body.append((addr, w, name))
            if name is None and w == 0:
                return addr
            if name in ("b", "bc") and not (w & 1):
                t = branch_target(addr, w, name)
                if start <= t < hard_end:
                    max_target = max(max_target, t)
            uncond = (name == "b" and not (w & 1)) or \
                     (name in ("bclr", "bcctr") and ((w >> 21) & 0x14) == 0x14 and not (w & 1))
            if uncond and name == "bcctr":
                # switch: os casos (e a tabela embutida) vêm depois do bctr
                tbl = self.find_jump_table(body, len(body) - 1, start, hard_end, count=False)
                if tbl:
                    max_target = max(max_target, max(tbl))
                    nxt = min(t for t in tbl if t > addr) if any(t > addr for t in tbl) else addr + 4
                    body = [(x,) + self.insn(x) for x in range(start, nxt, 4)]
                    addr = nxt
                    continue
            if uncond and addr + 4 > max_target:
                return addr + 4
            addr += 4
        return hard_end

    def region_of(self, addr):
        i = bisect.bisect_right(self.region_starts, addr) - 1
        s = self.region_starts[i]
        return s, self.regions[s]

    # ------------------------------------------------------------------
    def find_jump_table(self, body, k, start, end, count=True):
        """body: lista (addr, w, name) da rotina; k: índice do bctr."""
        lo = k
        while lo > 0 and k - lo < 48:
            pa, pw, pn = body[lo - 1]
            if pn == "b" or (pn in ("bclr", "bcctr") and ((pw >> 21) & 0x14) == 0x14):
                break
            lo -= 1
        mt = None
        for j in range(k - 1, lo - 1, -1):
            a_, w_, n_ = body[j]
            if n_ == "mtspr" and (((w_ >> 16) & 31) | (((w_ >> 11) & 31) << 5)) == 9:
                mt = j
                break
        if mt is None:
            return None
        seq = body[lo:k]
        sim = JumpTableSim(self.img)
        valid = lambda t: t is not None and not t & 3 and start <= t < end
        # 1) limite explícito: cmplwi crX, rI, N seguido de bc/bclr condicional em crX
        for j in range(mt - 1, lo - 1, -1):
            a_, w_, n_ = body[j]
            if n_ == "cmpli" and not (f_d(w_) & 1):
                crf, idx, n = f_d(w_) >> 2, f_a(w_), f_uimm(w_)
                if n > 4096:
                    return None
                used = any(n2 in ("bc", "bclr") and (f_a(w2) >> 2) == crf for _, w2, n2 in body[j + 1:k])
                if not used:
                    continue
                targets = []
                for i in range(n + 1):
                    t = sim.run(seq, idx, j - lo, i)
                    if not valid(t):
                        return None
                    targets.append(t)
                return targets
        # 2) sem limite visível (verificado noutro bloco): descobre o registo de índice e lê a
        #    tabela enquanto as entradas apontarem para dentro da rotina. O `default` do switch
        #    continua a cobrir índices não enumerados.
        for idx in range(32):
            t0, t1 = sim.run(seq, idx, 0, 0), sim.run(seq, idx, 0, 1)
            if not (valid(t0) and valid(t1)) or t0 == t1:
                continue
            targets = [t0, t1]
            for i in range(2, 512):
                t = sim.run(seq, idx, 0, i)
                if not valid(t):
                    break
                targets.append(t)
            if count:
                self.jt_unbounded += 1
            return targets
        return None

    # ------------------------------------------------------------------
    def emit_function(self, entry):
        rs, re_ = self.region_of(entry)
        end = re_
        body = []
        for addr in range(entry, end, 4):
            w, name = self.insn(addr)
            body.append((addr, w, name))
        labels = set()
        out = []
        name_of = lambda t: f"sub_{t:08X}"

        def jump(t):
            if entry <= t < end:
                labels.add(t)
                return f"goto loc_{t:08X};"
            if t in self.funcs:
                return f"{{ {name_of(t)}(ctx, mem_base); return; }}"
            self.unresolved_branches.append((entry, t))
            return f"{{ ppc_call_indirect(ctx, mem_base, 0x{t:08X}u); return; }}"

        def cond(bo, bi):
            pre, cs = "", []
            if not (bo & 4):
                pre = "--ctx.ctr; "
                cs.append("ctx.ctr == 0" if bo & 2 else "ctx.ctr != 0")
            if not (bo & 16):
                bit = f"ctx.cr[{bi >> 2}].{CRB[bi & 3]}"
                cs.append(bit if bo & 8 else f"!{bit}")
            return pre, " && ".join(cs)

        stats = self.stats
        for k, (addr, w, name) in enumerate(body):
            nxt = addr + 4
            code = None
            if name is None:
                self.unknown_words[f"op{w >> 26}"] += 1
                self.unimpl["<desconhecida>"] += 1
                code = f"ppc_unimplemented(ctx, mem_base, 0x{addr:08X}u, 0x{w:08X}u);"
            elif name in ("b", "bc"):
                t = branch_target(addr, w, name)
                lk = w & 1
                if name == "b":
                    pre, c = "", ""
                else:
                    pre, c = cond(f_d(w), f_a(w))
                if lk:
                    if t == nxt:
                        act = f"ctx.lr = 0x{nxt:08X}ull;"
                    elif t in self.funcs:
                        act = f"ctx.lr = 0x{nxt:08X}ull; {name_of(t)}(ctx, mem_base);"
                    else:
                        self.unresolved_branches.append((entry, t))
                        act = f"ctx.lr = 0x{nxt:08X}ull; ppc_call_indirect(ctx, mem_base, 0x{t:08X}u);"
                else:
                    act = jump(t)
                code = pre + (f"if ({c}) {{ {act} }}" if c else act)
            elif name == "bclr":
                pre, c = cond(f_d(w), f_a(w))
                if w & 1:
                    act = (f"{{ uint32_t t = (uint32_t)ctx.lr; ctx.lr = 0x{nxt:08X}ull; "
                           f"ppc_call_indirect(ctx, mem_base, t); }}")
                else:
                    act = "return;"
                code = pre + (f"if ({c}) {act}" if c else act)
            elif name == "bcctr":
                pre, c = cond(f_d(w) | 4, f_a(w))
                if w & 1:
                    act = f"{{ ctx.lr = 0x{nxt:08X}ull; ppc_call_indirect(ctx, mem_base, (uint32_t)ctx.ctr); }}"
                else:
                    tbl = self.find_jump_table(body, k, entry, end)
                    if tbl is not None:
                        self.jt_ok += 1
                        cases = []
                        for t in sorted(set(tbl)):
                            labels.add(t)
                            cases.append(f"  case 0x{t:08X}u: goto loc_{t:08X};")
                        act = ("switch ((uint32_t)ctx.ctr) {\n" + "\n".join(cases) +
                               "\n  default: ppc_call_indirect(ctx, mem_base, (uint32_t)ctx.ctr); return;\n  }")
                    else:
                        if c == "":
                            self.jt_fail.append(addr)
                        act = "{ ppc_call_indirect(ctx, mem_base, (uint32_t)ctx.ctr); return; }"
                code = pre + (f"if ({c}) {act}" if c else act)
            elif name == "rfi":
                code = None
            else:
                code = emit_int(name, w)
                if code is None and (name.startswith("f") or name in ("mffs", "mtfsf", "mtfsfi", "mtfsb0", "mtfsb1", "mcrfs")):
                    code = emit_fp(name, w)
                if code is None and (name.startswith(("v", "lv", "stv", "mfvscr", "mtvscr"))):
                    code = emit_vec(name, w)
            if code is None:
                key = name or "<desconhecida>"
                if name is not None:
                    self.unimpl[key] += 1
                    if len(self.unimpl_sites[key]) < 8:
                        self.unimpl_sites[key].append(f"0x{addr:08X}")
                code = f"ppc_unimplemented(ctx, mem_base, 0x{addr:08X}u, 0x{w:08X}u);"
            elif "{ADDR}" in code:
                code = code.replace("{ADDR}", f"0x{addr:08X}u")
            stats["insns"] += 1
            if name:
                stats["op:" + name] += 1
            out.append((addr, w, name, code))
        # Composição
        lines = [f"PPC_FUNC({name_of(entry)}) {{",
                 f"  // 0x{entry:08X}-0x{end:08X}" + ("" if entry == rs else f" (entrada no meio da rotina 0x{rs:08X})")]
        comments = self.args.comments
        for addr, w, name, code in out:
            if addr in labels:
                lines.append(f"loc_{addr:08X}:")
            if comments:
                mn = name or "?"
                if name in ("b", "bc", "bclr", "bcctr") and w & 1:
                    mn += "l"
                lines.append(f"  // {addr:08X} {w:08X} {mn}")
            lines.append("  " + code)
        # Queda pelo fim da região
        if end in self.funcs:
            lines.append(f"  {name_of(end)}(ctx, mem_base); return;")
        else:
            lines.append(f"  ppc_unreachable(ctx, mem_base, 0x{end:08X}u);")
        lines.append("}")
        return "\n".join(lines) + "\n\n"

    # ------------------------------------------------------------------
    def generate(self):
        out_dir = self.args.out
        os.makedirs(out_dir, exist_ok=True)
        for old in os.listdir(out_dir):
            if old.startswith("ppc_recomp_") and old.endswith(".cpp"):
                os.remove(os.path.join(out_dir, old))
        rt = os.path.join(os.path.dirname(os.path.abspath(__file__)), "recomp_runtime")
        for fn in ("ppc_context.h", "ppc_runtime.cpp", "ppc_smoke_main.cpp"):
            shutil.copyfile(os.path.join(rt, fn), os.path.join(out_dir, fn))

        all_funcs = sorted(self.funcs)
        hdr = ["// Gerado por tools/ppc_recomp.py — não editar.", "#pragma once", '#include "ppc_context.h"', ""]
        hdr += [f"PPC_FUNC(sub_{a:08X});" for a in all_funcs]
        self.write(os.path.join(out_dir, "ppc_funcs.h"), "\n".join(hdr) + "\n")

        # Corpos
        files = []
        buf, size, idx = [], 0, 0
        prelude = '// Gerado por tools/ppc_recomp.py — não editar.\n#include "ppc_funcs.h"\n\n'
        limit = self.args.file_size
        t0 = time.time()
        for n, e in enumerate(self.entries):
            text = self.emit_function(e)
            buf.append(text)
            size += len(text)
            if size >= limit:
                fn = f"ppc_recomp_{idx:03d}.cpp"
                self.write(os.path.join(out_dir, fn), prelude + "".join(buf))
                files.append(fn)
                buf, size, idx = [], 0, idx + 1
            if n % 2000 == 0:
                print(f"  {n}/{len(self.entries)} rotinas ({time.time() - t0:.0f}s)", flush=True)
        if buf:
            fn = f"ppc_recomp_{idx:03d}.cpp"
            self.write(os.path.join(out_dir, fn), prelude + "".join(buf))
            files.append(fn)
        self.files = files

        # Imports
        imp = ['// Gerado por tools/ppc_recomp.py — thunks de import (xam.xex = 0, xboxkrnl.exe = 1).',
               '#include "ppc_funcs.h"', ""]
        for t in sorted(self.imports):
            li, ordn, lname = self.imports[t]
            imp.append(f"PPC_FUNC(sub_{t:08X}) {{ ppc_import_call(ctx, mem_base, {li}, 0x{ordn:04X}); }}  // {lname}")
        imp.append("")
        imp.append("extern const PPCImportVar g_ppc_import_vars[] = {")
        for lname, ordn, rec in self.import_vars:
            li = 0 if lname.startswith("xam") else 1
            imp.append(f"  {{{li}, 0x{ordn:04X}, 0x{rec:08X}u}},  // {lname}")
        imp.append("};")
        imp.append(f"extern const uint32_t g_ppc_import_var_count = {len(self.import_vars)};")
        self.write(os.path.join(out_dir, "ppc_imports.cpp"), "\n".join(imp) + "\n")

        # Tabela de despacho
        tab = ['// Gerado por tools/ppc_recomp.py — tabela de despacho (ordenada por endereço).',
               '#include "ppc_funcs.h"', "", "extern const PPCFuncMapping g_ppc_func_table[] = {"]
        tab += [f"  {{0x{a:08X}u, sub_{a:08X}}}," for a in all_funcs]
        tab.append("};")
        tab.append(f"extern const uint32_t g_ppc_func_count = {len(all_funcs)};")
        tab.append(f"extern const uint32_t g_ppc_entry_point = 0x{self.entry_point:08X}u;")
        tab.append(f"extern const uint32_t g_ppc_image_base = 0x{self.img.base:08X}u;")
        tab.append(f"extern const uint32_t g_ppc_image_size = 0x{len(self.img.data):08X}u;")
        self.write(os.path.join(out_dir, "ppc_func_table.cpp"), "\n".join(tab) + "\n")

        self.write_cmake(out_dir, files)

    def write(self, path, text):
        with open(path, "w", encoding="utf-8", newline="\n") as fp:
            fp.write(text)

    def write_cmake(self, out_dir, files):
        tc = os.path.join(out_dir, "toolchain")
        os.makedirs(tc, exist_ok=True)
        for tool, sub in (("zig-cxx.cmd", "c++"), ("zig-cc.cmd", "cc"), ("zig-ar.cmd", "ar"), ("zig-ranlib.cmd", "ranlib")):
            with open(os.path.join(tc, tool), "w", newline="\r\n") as fp:
                fp.write(f"@echo off\npython -m ziglang {sub} %*\n")
        self.write(os.path.join(tc, "zig-toolchain.cmake"), """# Toolchain: Zig (Clang) via `python -m ziglang`
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_CXX_COMPILER "${CMAKE_CURRENT_LIST_DIR}/zig-cxx.cmd")
set(CMAKE_C_COMPILER "${CMAKE_CURRENT_LIST_DIR}/zig-cc.cmd")
set(CMAKE_AR "${CMAKE_CURRENT_LIST_DIR}/zig-ar.cmd" CACHE FILEPATH "" FORCE)
set(CMAKE_RANLIB "${CMAKE_CURRENT_LIST_DIR}/zig-ranlib.cmd" CACHE FILEPATH "" FORCE)
""")
        src_list = "\n".join(f"  {f}" for f in files)
        self.write(os.path.join(out_dir, "CMakeLists.txt"), f"""# Gerado por tools/ppc_recomp.py
# Configurar:  cmake -G Ninja -S generated_src -B build -DCMAKE_TOOLCHAIN_FILE=generated_src/toolchain/zig-toolchain.cmake -DCMAKE_BUILD_TYPE=Release
# Compilar:    cmake --build build
cmake_minimum_required(VERSION 3.20)
project(gow_ppc_recomp CXX)

# O código é C++20; std::byteswap (C++23 <bit>) é usado quando a biblioteca o fornece.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(PPC_OPT_FLAGS "-O1" CACHE STRING "Otimização das rotinas traduzidas")
set(PPC_RECOMP_SOURCES
{src_list}
)

add_library(ppc_recomp STATIC ${{PPC_RECOMP_SOURCES}} ppc_imports.cpp ppc_func_table.cpp)
target_include_directories(ppc_recomp PUBLIC ${{CMAKE_CURRENT_SOURCE_DIR}})
separate_arguments(_ppc_opt UNIX_COMMAND "${{PPC_OPT_FLAGS}}")
target_compile_options(ppc_recomp PRIVATE ${{_ppc_opt}} -fwrapv -fno-strict-aliasing
  -Wno-unused-label -Wno-unused-variable -Wno-unused-but-set-variable -Wno-unknown-warning-option)

add_executable(ppc_smoke ppc_runtime.cpp ppc_smoke_main.cpp)
target_link_libraries(ppc_smoke PRIVATE ppc_recomp)
target_compile_options(ppc_smoke PRIVATE -O1 -fwrapv -fno-strict-aliasing)

# Build isolado de generated_src/: inclui runtime/ também (a partir da raiz, o CMakeLists.txt raiz já o faz).
# O runtime real (loader, kernel/XAM HLE) vive em runtime/ e liga contra ppc_recomp.
# Não deve incluir ppc_runtime.cpp (stub do smoke), que define os mesmos ganchos.
if(PROJECT_IS_TOP_LEVEL AND EXISTS "${{CMAKE_CURRENT_SOURCE_DIR}}/../runtime/CMakeLists.txt")
  add_subdirectory("${{CMAKE_CURRENT_SOURCE_DIR}}/../runtime" runtime)
endif()
""")

    def report(self, path):
        ops = {k[3:]: v for k, v in self.stats.items() if k.startswith("op:")}
        rep = {
            "generated_at": time.strftime("%Y-%m-%dT%H:%M:%S"),
            "image": self.args.image,
            "regions_pdata": self.stats["regions_pdata"],
            "regions_synthetic": self.stats["regions_synthetic"],
            "regions_pointer_seeded": self.stats["regions_pointer_seeded"],
            "entries_pointer_seeded_mid": self.stats["entries_pointer_seeded_mid"],
            "entries": self.stats["entries"],
            "entries_mid_region": self.stats["entries_mid_region"],
            "import_thunks": self.stats["import_thunks"],
            "regions_extended_for_switch": self.stats["regions_extended_for_switch"],
            "dispatch_table_size": len(self.funcs),
            "translated_instructions": self.stats["insns"],
            "unimplemented_total": sum(self.unimpl.values()),
            "unimplemented": dict(self.unimpl.most_common()),
            "unimplemented_sites": {k: v for k, v in self.unimpl_sites.items()},
            "unknown_by_primary_opcode": dict(self.unknown_words.most_common()),
            "jump_tables_resolved": self.jt_ok,
            "jump_tables_without_visible_bound": self.jt_unbounded,
            "bctr_unresolved": len(self.jt_fail),
            "bctr_unresolved_sites": [f"0x{a:08X}" for a in self.jt_fail[:200]],
            "unresolved_direct_branches": len(self.unresolved_branches),
            "unresolved_direct_branch_sites": [f"0x{a:08X}->0x{t:08X}" for a, t in self.unresolved_branches[:200]],
            "output_files": len(self.files),
            "warnings": self.warnings[:200],
            "mnemonic_histogram": dict(sorted(ops.items(), key=lambda kv: -kv[1])),
        }
        with open(path, "w", encoding="utf-8") as fp:
            json.dump(rep, fp, indent=2)
        return rep


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--image", default="extracted/base_image.bin")
    ap.add_argument("--map", default="analysis/binary_map.json")
    ap.add_argument("--pdata", default="analysis/pdata_functions.json")
    ap.add_argument("-o", "--out", default="generated_src")
    ap.add_argument("--report", default="analysis/recomp_report.json")
    ap.add_argument("--file-size", type=int, default=1_500_000, help="bytes aproximados por .cpp")
    ap.add_argument("--no-comments", dest="comments", action="store_false",
                    help="não emitir o comentário de disassembly por instrução")
    args = ap.parse_args()
    t0 = time.time()
    rc = Recompiler(args)
    print("[1/3] Mapeamento de rotinas (CFG)...", flush=True)
    rc.build_cfg()
    print(f"      {rc.stats['entries']} pontos de entrada ({rc.stats['regions_pdata']} .pdata, "
          f"{rc.stats['regions_synthetic']} regiões sintéticas, {rc.stats['entries_mid_region']} entradas internas), "
          f"{rc.stats['import_thunks']} thunks de import", flush=True)
    print("[2/3] Emissão de C++...", flush=True)
    rc.generate()
    print("[3/3] Relatório...", flush=True)
    rep = rc.report(args.report)
    print(f"      {rep['translated_instructions']} instruções, {rep['unimplemented_total']} não implementadas, "
          f"{rep['jump_tables_resolved']} tabelas de salto, {rep['bctr_unresolved']} bctr dinâmicos, "
          f"{rep['output_files']} ficheiros .cpp — {time.time() - t0:.0f}s", flush=True)


if __name__ == "__main__":
    main()
