#!/usr/bin/env bash
# Jugar PC Fútbol 2001 recompilado con ventana y sonido (Windows: Git Bash). Arma el disco C: recién instalado la primera
# vez (generated/pcf2001/c_base, de pcf2001_preparar.sh) y lo conserva entre partidas.
# F12 guarda una captura en generated/pcf2001/run/shots. Cerrar la ventana termina el juego.
# Uso: tools/pcf2001_jugar.sh [escala=1]
cd "$(dirname "$0")/.."
C=generated/pcf2001/run/jugar
mkdir -p "$C" generated/pcf2001/run/shots
[ -d "$C/PCF2001" ] || cp -r generated/pcf2001/c_base/. "$C/"
export RECOMP_SCALE="${1:-1}"
export RECOMP_ESTIRAR="${RECOMP_ESTIRAR:-1}"   # F11 / Alt+Enter: pantalla completa estirada (0 = con barras)
export RECOMP_SHOT_DIR=generated/pcf2001/run/shots
./generated/pcf2001/build/pcf2001_recomp.exe generated/pcf2001/build/images generated/pcf2001/cd "$C"
