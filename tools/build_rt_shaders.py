#!/usr/bin/env python3
"""Compila runtime/gpu/rt_shaders.hlsl (DXR 1.1, SM 6.5) com o dxc do Windows SDK e grava runtime/gpu/rt_shaders.inc.

O DXIL fica embutido no gow.exe: o jogo não precisa do dxcompiler.dll. Só é preciso correr isto quando o .hlsl muda.
  python tools/build_rt_shaders.py [--dxc caminho\\dxc.exe]
O dxil.dll ao lado do dxc.exe assina o DXIL (sem assinatura o D3D12 recusa-o fora do modo de programador).
"""
import argparse
import glob
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "runtime", "gpu", "rt_shaders.hlsl")
OUT = os.path.join(ROOT, "runtime", "gpu", "rt_shaders.inc")
ENTRIES = [("kRtVs", "vs_main", "vs_6_5"), ("kRtPs", "ps_main", "ps_6_5")]


def find_dxc():
    kits = sorted(glob.glob(r"C:\Program Files (x86)\Windows Kits\10\bin\10.*\x64\dxc.exe"))
    return kits[-1] if kits else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dxc", default=find_dxc())
    args = ap.parse_args()
    if not args.dxc or not os.path.isfile(args.dxc):
        sys.exit("dxc.exe não encontrado (Windows SDK); use --dxc")
    out = ["// Gerado por tools/build_rt_shaders.py a partir de rt_shaders.hlsl. Não editar.\n"]
    with tempfile.TemporaryDirectory() as tmp:
        for name, entry, target in ENTRIES:
            obj = os.path.join(tmp, name + ".dxil")
            r = subprocess.run([args.dxc, "-nologo", "-T", target, "-E", entry, "-O3", "-Fo", obj, SRC],
                               capture_output=True, text=True)
            if r.returncode != 0:
                sys.exit(f"dxc falhou em {entry}:\n{r.stdout}{r.stderr}")
            data = open(obj, "rb").read()
            if data[:4] != b"DXBC":
                sys.exit(f"{entry}: saída inesperada")
            out.append(f"static const unsigned char {name}[{len(data)}] = {{\n")
            for i in range(0, len(data), 24):
                out.append("  " + ", ".join(f"0x{b:02X}" for b in data[i:i + 24]) + ",\n")
            out.append("};\n")
            print(f"{entry}: {len(data)} bytes")
    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.writelines(out)
    print("gravado", OUT)


if __name__ == "__main__":
    main()
