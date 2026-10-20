# Cómo funciona

## El método

1. **Traducir** (`tools/recomp/build.py`). Se cargan el exe y las DLL del juego en las direcciones del perfil
   (`config/juegos/pcf2001.json`: el exe en 0x400000 y las DLL rebasadas de 0xC00000 en adelante, sin pisarse), se
   buscan las funciones a partir de los puntos de entrada y de los punteros a código, y cada función x86 se escribe como
   una función C que opera sobre un `Cpu` (registros, banderas, FPU, MMX) y una memoria plana de 32 bits. El
   analizador (`analysis.py`) y el escritor base (`translate.py`) hacen lo común; `translator.py` agrega lo que el 2001
   necesitó. La traducción va en dos pasadas para acotar la memoria (~1,4 GB).
2. **Compilar** (`tools/recomp/compile.py`). El C generado (`generated/pcf2001/build/gen`, ~1.300 archivos) más el
   runtime (`src/rt`) se compilan con clang y se enlazan contra SDL2. Es incremental: sólo lo que cambió.
3. **Correr.** `main.c` arma la memoria, carga las imágenes (`src/rt/loader.c`), monta el disco C: virtual
   (`generated/pcf2001/run/...`) y el CD (`generated/pcf2001/cd`), y salta al punto de entrada de cada DLL y del exe.
   Cada llamada a Windows cae en una función del runtime (`RT_IMPORT(Nombre)`).

Las direcciones que aparecen en la documentación (0xfaad50, 0x49b500…) son direcciones del juego en ese mapa de
memoria; en el C generado cada instrucción aparece como comentario `/* <dirección>: <instrucción> */`.

## El runtime (`src/rt`)

| Archivo | Imita |
|---|---|
| `core/rt.c`, `core/heap.c`, `core/host/*` | CPU, llamadas y saltos indirectos, heap del juego, archivos y reloj del host |
| `loader.c`, `main.c` | carga de imágenes, imports, perfil del juego, arranque |
| `cpu_ext.c` | instrucciones que el traductor delega (CPUID, `fsincos`…) |
| `win/kernel32.c`, `files.c`, `handles.c`, `threads.c`, `seh.c` | procesos, archivos y disco virtual, handles, hilos cooperativos, excepciones (SEH y throw de C++) |
| `win/user32.c`, `input.c`, `screen.c`, `cursor.c`, `gdi32.c` | ventanas y mensajes, teclado (con códigos de escaneo) y mouse, la ventana SDL, cursores, GDI |
| `win/ddraw.c`, `ddraw_int.h` | DirectDraw 1/2/4/7: superficies, paletas, color alto (16/32 bits), z-buffer, blits |
| `win/d3d.c` | Direct3D 7 «RGB Emulation»: **rasterizador por software** (transformación, recorte, texturas, z-buffer, mezcla, alfa, color clave, vertex buffers) |
| `win/dsound.c`, `dsound_int.h`, `audio.c` | DirectSound (buffers, posición, volumen, paneo) y el mezclador SDL a 44,1 kHz |
| `win/dshow.c` | DirectShow mínimo para la música (FilterGraph + MP3 con minimp3) |
| `win/com.c`, `com.h` | objetos COM, ole32 (CoCreateInstance, OLE de MFC), DirectMusic mínimo |
| `win/oleaut32.c`, `webbrowser.c` | BSTR/VARIANT; un control WebBrowser falso (el panel de ayuda; no dibuja) |
| `win/winmm.c` | tiempo, joystick (no hay), mmio (WAV desde archivo o memoria) |
| `win/locale.c`, `misc32.c`, `win9x.c`, `resources.c` | código de página, utilidades, variantes A/W, recursos del PE |

## Versión web

`tools/recomp/compile_wasm.py --target web` compila el mismo C con Emscripten. Las esperas del juego (mensajes,
eventos, hilos) van sobre Asyncify, y el CD se lee por `fetch` con rangos (`src/rt/core/host/webcd.c`, índice de
`tools/make_cd_index.py`). Las imágenes traducidas y la instalación (`c_base`) van en el `.data` empaquetado; la
página (`web/index.html`) copia la instalación a `/c` (IndexedDB) la primera vez. Las variables de entorno se pasan
por la URL: `?trace=1`, `?perf=1`, `?yield=1`. `--target node` corre lo mismo en Node, sin pantalla, para probar.

## Variables de entorno

| Variable | Efecto |
|---|---|
| `RECOMP_TRACE=1` / `=2` | traza de llamadas a Windows / además cada método COM con su resultado y quién lo llamó |
| `RECOMP_TRACE_KEYS=1` | sólo el teclado (mensajes y códigos de escaneo) |
| `RECOMP_HEADLESS=1` | sin ventana (sin audio) |
| `RECOMP_SCALE=n` | escala de la ventana |
| `RECOMP_ESTIRAR=1` | la imagen llena la ventana o la pantalla completa aunque se deforme (`pcf2001_jugar.sh` la prende) |
| `RECOMP_MUTE=1` | sin audio |
| `RECOMP_SHOT_DIR=dir` | carpeta de las capturas de F12; `RECOMP_SHOT_EVERY=n` captura cada n cuadros |
| `RECOMP_FIXED_CLOCK=1` | reloj virtual (corridas reproducibles; sin audio) |
| `RECOMP_SCRIPT=archivo` | guion de entrada (clics, teclas, esperas); `RECOMP_SALIR=1` sale al terminarlo |
| `RECOMP_SIN_CD=1` | sin unidad de CD (todo desde C:) |
| `RECOMP_WATCHDOG`, `RECOMP_DUMP_AFTER_MS`, `RECOMP_PERF`, `RECOMP_YIELD`, `RECOMP_CPUID_1`, `RECOMP_HEAP_CERO` | depuración fina; ver el código que las lee (`grep -rn getenv src/rt`) |

## Cómo se depura un crash

Los mensajes de parada del runtime dicen qué pasó:

- **`call to 0x…, which is not a recompiled function (from 0x…)`**: se saltó a código que el traductor no encontró. Si el
  origen es `0x40000000`, la llamada vino del runtime (un callback: ventana, hilo, manejador de excepción). Buscar la
  dirección en el C generado (`grep -n "0x<dir>u" generated/pcf2001/build/gen/*.c`) muestra quién la usa; si es un
  patrón del compilador que el traductor no reconoce, se agrega en `build.py` (así entraron los manejadores SEH).
  **0x11000000** en particular es `mem[4]` (StackBase del TEB): sale al llamar por una vtable o una tabla nula, por
  ejemplo un objeto COM ya liberado.
- **`import X has no replacement`**: falta imitar esa función de Windows (`RT_IMPORT(X)` en el archivo que corresponda).
- **`método COM sin reemplazo: Interfaz::Método`**: falta ese método en la vtable imitada.
- **`clase no registrada`** (en la traza): el juego pidió un objeto COM que no imitamos (`CoCreateInstance` en `com.c`).

Herramientas: `tools/recomp/desensamblar.py` (una zona de una imagen), `leer_tabla.py` (tablas de saltos),
`sonda.py` (inserta un `fprintf` de registros en el C generado), `pe_info.py` (tamaños y bases de las imágenes).
Con `RECOMP_TRACE=2` se ve en orden qué pidió el juego y qué le devolvimos; casi todos los arreglos salieron de ahí.

## El ciclo de un arreglo

1. Reproducir sin ventana: `RECOMP_TRACE=2 bash tools/pcf2001_correr.sh 60` y leer el final de
   `generated/pcf2001/run/log.txt`. La última llamada antes de la parada suele ser la causa. Lo que sólo pasa jugando
   (un clic, una pantalla) se reproduce con ventana: `RECOMP_TRACE=1 bash tools/pcf2001_jugar.sh > log.txt 2>&1`.
2. Ubicar el código del juego que llamó: la dirección `desde 0x…` de la traza COM, o la dirección de retorno, se
   busca en el C generado. Cada instrucción original está como comentario.
3. Arreglar en el runtime (`src/rt/win/*.c`), o en el traductor si el problema es de traducción (`build.py`,
   `translator.py`).
4. Recompilar. Un cambio del runtime recompila sólo el runtime. Un cambio del traductor exige retraducir, aunque
   `build.py` sólo reescribe los archivos de C que cambian.
5. Verificar con la misma corrida. Después, jugando, que lo que ya andaba siga andando: menú, pretemporada, partido en
   los dos modos, sonido.

Criterio: imitar lo que hace Windows, no parchear el juego. Si el juego hace algo raro, primero confirmar con el
desensamblado que de verdad lo pide así. Una rareza del original se imita tal cual.

## Notas prácticas

- Traducir pide ~1,4 GB de RAM; compilar desde cero, ~40 min con 1 proceso. Con más procesos va más rápido, pero cada
  clang de un archivo grande usa bastante memoria.
- Los parches de C que se arman desde un script (heredoc de bash o Python) pueden convertir los `\n` de una cadena C
  en saltos de línea reales: revisar el diff antes de compilar.
- El C generado es obra derivada del juego: vive en `generated/` y nunca se commitea.
