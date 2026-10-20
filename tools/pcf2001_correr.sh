#!/usr/bin/env bash
# PC Fútbol 2001 recompilado: arma un disco C: recién instalado (generated/pcf2001/c_base, de pcf2001_preparar.sh) y lo
# corre. Sin pantalla con traza por defecto; RECOMP_HEADLESS=0 para ventana. Las variables RECOMP_* pasan tal cual.
# Uso: tools/pcf2001_correr.sh [segundos=60] [carpeta C:=generated/pcf2001/run/c]
set -u
cd "$(dirname "$0")/.."
SECS="${1:-60}"
C="${2:-generated/pcf2001/run/c}"
export RECOMP_TRACE="${RECOMP_TRACE:-1}" RECOMP_HEADLESS="${RECOMP_HEADLESS:-1}"
mkdir -p "$C" generated/pcf2001/run
[ -d "$C/PCF2001" ] || cp -r generated/pcf2001/c_base/. "$C/"
timeout "$SECS" ./generated/pcf2001/build/pcf2001_recomp.exe generated/pcf2001/build/images generated/pcf2001/cd "$C" \
    > generated/pcf2001/run/log.txt 2>&1
echo "exit=$? líneas=$(wc -l < generated/pcf2001/run/log.txt)"
