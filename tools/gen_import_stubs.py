#!/usr/bin/env python3
"""gen_import_stubs.py — gera os stubs de imports do runtime a partir de analysis/import_names.json.

Grava:
  runtime/import_bindings.inc   {lib, ordinal, nome, é_variável, slot, thunk} para o loader/dispatcher
  runtime/imports_stubs.cpp     um XBOX_IMPORT_STUB(nome) com STUBBED_IMPORT("nome") para cada função
                                importada que ainda NÃO tem implementação real em runtime/ (XBOX_IMPORT(nome))
Rodar de novo sempre que uma função ganhar implementação real: o stub correspondente some.

Uso: python tools/gen_import_stubs.py [--json analysis/import_names.json] [--runtime runtime]
"""
import argparse
import json
import os
import re

IMPL = re.compile(r"^\s*XBOX_IMPORT\(\s*(\w+)\s*\)", re.M)
STUBS = "imports_stubs.cpp"


def implemented(runtime_dir):
    names = {}
    for root, _, files in os.walk(runtime_dir):
        for fn in files:
            if fn == STUBS or not fn.endswith((".cpp", ".h")):
                continue
            path = os.path.join(root, fn)
            with open(path, encoding="utf-8", errors="replace") as fp:
                for m in IMPL.finditer(fp.read()):
                    names[m.group(1)] = os.path.relpath(path, runtime_dir)
    return names


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--json", default="analysis/import_names.json")
    ap.add_argument("--runtime", default="runtime")
    a = ap.parse_args()
    rows = json.load(open(a.json, encoding="utf-8"))["imports"]
    impl = implemented(a.runtime)

    with open(os.path.join(a.runtime, "import_bindings.inc"), "w", encoding="utf-8", newline="\n") as fp:
        fp.write("// Gerado por tools/gen_import_stubs.py — {lib, ordinal, nome, variável, slot, thunk}\n")
        for r in rows:
            slot = int(r["variable_address"] or "0", 16)
            thunk = int(r["thunk_address"] or "0", 16)
            fp.write(f'{{{r["lib_index"]}, 0x{r["ordinal"]:04X}, "{r["name"]}", '
                     f'{"true" if r["type"] == "variable" else "false"}, 0x{slot:08X}u, 0x{thunk:08X}u}},\n')

    funcs = [r for r in rows if r["type"] == "function"]
    stubs = [r for r in funcs if r["name"] not in impl]
    out = ["// Gerado por tools/gen_import_stubs.py — não editar.",
           "// Stubs das funções importadas sem implementação real. Para implementar uma função, defina",
           "// XBOX_IMPORT(Nome) noutro ficheiro de runtime/ e rode o gerador de novo (o stub some daqui).",
           "// Convenção: STUBBED_IMPORT regista a chamada; o stub devolve 0 em r3 (STATUS_SUCCESS /",
           "// ERROR_SUCCESS / NULL, conforme a função).",
           '#include "xbox_import.h"', ""]
    for lib in ("xboxkrnl.exe", "xam.xex"):
        sel = sorted((r for r in stubs if r["lib"] == lib), key=lambda r: r["ordinal"])
        out.append(f"// ---- {lib}: {len(sel)} stubs ----")
        for r in sel:
            out.append(f'XBOX_IMPORT_STUB({r["name"]}) {{ STUBBED_IMPORT("{r["name"]}"); ctx.r[3] = 0; }}'
                       f'  // ordinal {r["ordinal_hex"]}')
        out.append("")
    with open(os.path.join(a.runtime, STUBS), "w", encoding="utf-8", newline="\n") as fp:
        fp.write("\n".join(out))
    print(f"{len(funcs)} funções importadas: {len(funcs) - len(stubs)} implementadas, {len(stubs)} stubs; "
          f"{len(rows) - len(funcs)} variáveis")
    for name, where in sorted(impl.items()):
        print(f"  implementada: {name} ({where})")


if __name__ == "__main__":
    main()
