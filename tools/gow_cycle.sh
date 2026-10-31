#!/usr/bin/env bash
# gow_cycle.sh — ciclo de diagnóstico: regenera stubs, compila o gow.exe (ninja -j 8) e executa-o com timeout.
#   tools/gow_cycle.sh [segundos] [log]      (padrão: 30 s, analysis/gow_run.log)
set -u
cd "$(dirname "$0")/.."
PY=${GOW_PYTHON_DIR:-$HOME/AppData/Local/Python/pythoncore-3.14-64}
export PATH="$PY/Scripts:$PY/Lib/site-packages/cmake/data/bin:$PATH"
secs=${1:-30}
log=${2:-analysis/gow_run.log}

python tools/gen_import_stubs.py | head -1
if ! cmake --build build --target gow -- -j 8 > analysis/build_gow.log 2>&1; then
  grep -E 'error|undefined' analysis/build_gow.log | head -40
  exit 1
fi
GOW_WATCHDOG=${GOW_WATCHDOG:-$((secs-2))} timeout "$secs" ./build/runtime/gow.exe > "$log" 2>&1
rc=$?
echo "gow.exe saiu com código $rc (124 = timeout)"
grep -E '^\[import\] #' "$log" | wc -l | sed 's/^/imports distintos chamados: /'
grep -vE '^\[import\] ( |#)' "$log" | tail -25
