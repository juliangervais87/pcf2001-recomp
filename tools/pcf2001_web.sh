#!/usr/bin/env bash
# PC Fútbol 2001 en el navegador: arma el índice del CD, compila a WebAssembly y levanta el servidor local.
# Requiere generated/pcf2001 preparado (pcf2001_preparar.sh) y traducido (build.py), y Emscripten en third_party/emsdk
# (o en la variable EMSDK). Uso: tools/pcf2001_web.sh [puerto=8097] [jobs=1]
set -eu
cd "$(dirname "$0")/.."
PY="${PYTHON:-python}"
"$PY" tools/make_cd_index.py generated/pcf2001/cd generated/pcf2001/cd_index.json
"$PY" tools/recomp/compile_wasm.py --build generated/pcf2001/build --target web --jobs "${2:-1}"
# la animación de la pantalla de carga: el cursor de espera del juego (logo de Dinamic), de la instalación
"$PY" tools/cursor_carga.py generated/pcf2001/c_base/PCF2001/CURSORES/WaitOld.ani generated/pcf2001/build/wasm-web/carga.png
echo "abrir http://127.0.0.1:${1:-8097}/ (Ctrl+C corta el servidor)"
"$PY" tools/servir_web.py "${1:-8097}"
