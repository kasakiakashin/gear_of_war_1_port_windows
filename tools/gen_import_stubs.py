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
import sys

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
    # Outra versão do executável (ex.: a title update 5): os nomes por ordinal são os mesmos, mas as entradas da IAT e os
    # thunks mudam de sítio. --map binary_map.json dessa versão + --bindings ficheiro de saída (só esse é escrito).
    ap.add_argument("--map", help="binary_map.json de outra versão: endereços de slot/thunk")
    ap.add_argument("--bindings", default="import_bindings.inc")
    a = ap.parse_args()
    rows = json.load(open(a.json, encoding="utf-8"))["imports"]
    impl = implemented(a.runtime)
    if a.map:
        addr = {}
        for lib in json.load(open(a.map, encoding="utf-8"))["imports"]:
            for i in lib["imports"]:
                addr[(lib["library_index"], i["ordinal"])] = i
        missing = [r["name"] for r in rows if (r["lib_index"], r["ordinal"]) not in addr]
        if missing or len(addr) != len(rows):
            sys.exit(f"imports diferentes entre as versões: {missing[:5]} ({len(addr)} contra {len(rows)})")
        rows = [dict(r, variable_address=addr[(r["lib_index"], r["ordinal"])]["variable_address"],
                     thunk_address=addr[(r["lib_index"], r["ordinal"])]["thunk_address"]) for r in rows]

    with open(os.path.join(a.runtime, a.bindings), "w", encoding="utf-8", newline="\n") as fp:
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
