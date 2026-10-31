#!/usr/bin/env python3
"""import_names.py — dá nome aos ordinais importados (xboxkrnl.exe / xam.xex).

Lê as tabelas públicas de exports do Xenia (xboxkrnl_table.inc, xam_table.inc, licença BSD) apenas como
dados (regex), cruza com os imports de analysis/binary_map.json e grava:
  analysis/import_names.json   ordinal -> nome, tipo, endereço do thunk/variável
  runtime/import_table.inc      tabela C++ {lib, ordinal, nome, é_variável}
Confere que o tipo (função/variável) de cada import coincide com o da tabela.
"""
import argparse
import json
import re

PAT = re.compile(r"XE_EXPORT\(\s*(\w+)\s*,\s*(0x[0-9A-Fa-f]+)\s*,\s*(\w+)\s*,\s*(\w+)\s*\)")


def load(path):
    out = {}
    with open(path, encoding="utf-8", errors="replace") as fp:
        for m in PAT.finditer(fp.read()):
            out[int(m.group(2), 16)] = (m.group(3), m.group(4))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--kernel-table", required=True)
    ap.add_argument("--xam-table", required=True)
    ap.add_argument("--map", default="analysis/binary_map.json")
    ap.add_argument("--json", default="analysis/import_names.json")
    ap.add_argument("--inc", default="runtime/import_table.inc")
    a = ap.parse_args()
    tables = {"xboxkrnl.exe": load(a.kernel_table), "xam.xex": load(a.xam_table)}
    bm = json.load(open(a.map, encoding="utf-8"))
    rows, mism, missing = [], [], []
    for lib in bm["imports"]:
        t = tables[lib["name"]]
        for imp in lib["imports"]:
            o = imp["ordinal"]
            name, kind = t.get(o, (f"{lib['name'].split('.')[0]}_{o:04X}", "?"))
            if kind == "?":
                missing.append((lib["name"], o))
            isvar = imp["type"] == "variable"
            if kind != "?" and (kind == "kVariable") != isvar:
                mism.append((lib["name"], o, name))
            rows.append({"lib": lib["name"], "lib_index": lib["library_index"], "ordinal": o,
                         "ordinal_hex": f"0x{o:04X}", "name": name, "type": imp["type"],
                         "thunk_address": imp.get("thunk_address"), "variable_address": imp.get("variable_address")})
    json.dump({"source": "Xenia export tables (BSD), cruzadas com binary_map.json", "type_mismatches": mism,
               "unnamed": missing, "imports": rows}, open(a.json, "w", encoding="utf-8"), indent=1)
    with open(a.inc, "w", encoding="utf-8", newline="\n") as fp:
        fp.write("// Gerado por tools/import_names.py — {lib, ordinal, nome, variável}\n")
        for r in rows:
            fp.write(f'{{{r["lib_index"]}, 0x{r["ordinal"]:04X}, "{r["name"]}", {"true" if r["type"] == "variable" else "false"}}},\n')
    print(f"{len(rows)} imports, {len(missing)} sem nome, {len(mism)} com tipo divergente")
    for r in rows:
        print(f'{r["lib"][:3]} {r["ordinal_hex"]} {r["type"][0]} {r["name"]}')


if __name__ == "__main__":
    main()
