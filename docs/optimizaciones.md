# Optimizaciones recomendadas

Líneas a perseguir para que el juego corra sin cortes, sobre todo en la versión web: sonido que se corta en las cargas,
tirones al empezar una canción, cargas lentas. **Ninguna está medida todavía**: son hipótesis ordenadas por la relación
entre lo que se espera ganar y lo que cuestan. Cada una se mide antes y después: el perfil de la web con `?perf=1` y el
log del juego. Una vez hecha, se mueve a la tabla de problemas resueltos de `ficha-tecnica.md`.

## Contexto

En nativo, el audio de SDL corre en su propio hilo: una carga larga del juego no lo frena. En la web corre todo en un
solo hilo: el juego, la mezcla de audio (`audio_pump`, ~150 ms de cola) y las lecturas del CD por `fetch`, con
Asyncify para las esperas. Si el juego no le devuelve el control al navegador a tiempo, la cola de audio se vacía y se
oye el corte.

## Filas

| # | Qué | Ataca | Costo | Dónde | Cómo medirlo |
|---|---|---|---|---|---|
| O1 | Subir la cola de audio de la web de ~150 ms a 300-500 ms, y el retraso de lectura del mezclador (`lag_ms`) en la misma medida | cortes cortos en cargas | muy bajo | `src/rt/win/audio.c` (`audio_pump`, `lag_ms` en web) | contar los vaciados de la cola (`SDL_GetQueuedAudioSize` en 0) por minuto, antes y después |
| O2 | Decodificar la música de a tramos (a medida que el renderer pide muestras) o en un Web Worker, en vez de la pista entera en `Render` | tirón al empezar cada canción | bajo | `src/rt/win/dshow.c` (`decode_source`, `deliver`) | tiempo de `IGraphBuilder::Render` y del primer `Run` con `?perf=1` |
| O3 | Asyncify sólo donde hace falta (`ASYNCIFY_ONLY`/`ASYNCIFY_REMOVE`): las funciones que de verdad pueden llegar a una espera, medidas con una traza propia de qué funciones corren | velocidad general, tamaño del `.wasm`, memoria y tiempo del enlace (medido: con `-O2`, `wasm-opt` pasó los 10,6 GB en 40 min sin terminar) | medio | `tools/recomp/compile_wasm.py` y una traza propia de funciones | tamaño del `.wasm`, cuadros por segundo en el visionado, pico del enlace |
| O4 | Ceder al navegador también durante las esperas de archivos y las cargas largas, no sólo en el bucle de mensajes | cortes en cargas | bajo-medio | `src/rt/win/kernel32.c` y `files.c` (cesiones), `src/rt/core/host/webcd.c` | igual que O1 |
| O5 | Caché del CD en el navegador (Cache API o IndexedDB) con lectura anticipada en bloques más grandes, y precarga al inicio de los paquetes que siempre se usan (`SFX\GUISND`, `SFX\Musicas`, `GRAFICOS.PAK`) | cargas lentas; esperas de red en medio del juego | medio | `src/rt/core/host/webcd.c`, `web/index.html` | bytes y pedidos por pantalla (pestaña de red), tiempo de carga del menú y del partido |
| O6 | Compilar con más optimización lo caliente: el rasterizador (`d3d.c`) y el mezclador a `-O2`/`-O3` con SIMD de wasm (`-msimd128`), y los archivos generados de las funciones más usadas (medidas con una traza propia) a `-O2`; el resto queda en `-O1` | cuadros por segundo del visionado 3D y de los menús | medio | `tools/recomp/compile.py`, `compile_wasm.py` (opciones por archivo) | cuadros por segundo, tiempo por cuadro con `?perf=1` |
| O7 | Fijar la memoria inicial del wasm (`INITIAL_MEMORY`) en lo que el juego llega a usar, para evitar que crezca en medio de una carga | pausas puntuales | muy bajo | `tools/recomp/compile_wasm.py` | `performance.memory` y pausas largas en el perfil |
| O8 | Mezclar el audio en un AudioWorklet (hilo de audio del navegador) con un búfer circular en memoria compartida | cortes de audio en cualquier situación | alto (requiere COOP/COEP en el servidor y SharedArrayBuffer) | `src/rt/win/audio.c`, `web/index.html`, servidor | igual que O1, bajo carga sostenida |
| O9 | Enlace web más liviano: `wasm-opt` a `-O1` (o `-O2` sólo al publicar) mientras se desarrolla. **Hoy es necesario**: con `-O2` el enlace no termina en una máquina de 32 GB | memoria y tiempo del enlace en la máquina de desarrollo | muy bajo | `compile_wasm.py` (`--link-opt`) | pico de memoria y tiempo del enlace; ojo: V8 limita a 50.000 locales por función, y `-O2` es el que los fusiona |

## Orden sugerido

O1 y O2 primero: son baratas y atacan lo que más se nota. Después O3, por velocidad, tamaño y por el enlace. O8 queda
para cuando haga falta audio sin cortes en cualquier situación.
