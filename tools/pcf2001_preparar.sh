#!/usr/bin/env bash
# PC Fútbol 2001: arma desde la ISO lo que el traductor y el runtime necesitan, todo en generated/pcf2001 (ignorado).
#   cd/          el contenido de la ISO (la unidad del CD)
#   instalado/   lo que trae el instalador InstallShield (DATA1.CAB + DATA2.CAB), sacado con unshield en WSL
#   input/       las imágenes que se traducen (exe y DLL del instalador)
#   c_base/      el disco C: recién instalado: C:\PCF2001 con lo que el instalador copia (la carpeta Root)
# Uso: tools/pcf2001_preparar.sh <ruta de PCF_2001.iso>   (Windows con Git Bash, 7-Zip y WSL Ubuntu con unshield)
set -eu
cd "$(dirname "$0")/.."
ISO="${1:?falta la ruta de la ISO del PC Fútbol 2001}"
Z="/c/Program Files/7-Zip/7z.exe"
D=generated/pcf2001
mkdir -p "$D/cabs" "$D/input" "$D/c_base/PCF2001"
[ -f "$D/cd/DISK.ID" ] || "$Z" x -y -o"$D/cd" "$ISO" > /dev/null
cp "$D/cd/DATA1.CAB" "$D/cd/DATA1.HDR" "$D/cd/DATA2.CAB" "$D/cabs/"
W=$(wsl -d Ubuntu -- wslpath -a "$(pwd -W)" 2>/dev/null | tr -d '\r')    # la carpeta del repo vista desde WSL
[ -d "$D/instalado/Root" ] || wsl -d Ubuntu -- bash -lc "cd '$W/$D/cabs' && unshield -d ../instalado x DATA1.CAB > /dev/null"
for f in Shared_DLLs/MSVCRT.DLL Shared_DLLs/MSVCP60.DLL Shared_DLLs/MFC42.DLL "Exe_&_Dll/utils.dll" "Exe_&_Dll/gfxwin.dll" \
         "Exe_&_Dll/d2s2.dll" "Exe_&_Dll/dcd3d.dll" "Exe_&_Dll/DCSF.dll" "Exe_&_Dll/dcsfx.dll" "Exe_&_Dll/manager.exe"; do
    cp "$D/instalado/$f" "$D/input/"
done
cp -r "$D/instalado/Root/." "$D/c_base/PCF2001/"
echo "listo: $(ls "$D/input" | wc -l) imágenes, CD $(du -sh "$D/cd" | cut -f1), instalación $(du -sh "$D/c_base" | cut -f1)"
