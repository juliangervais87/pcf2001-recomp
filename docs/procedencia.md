# Procedencia del código de terceros

| Origen | Commit | Qué | Dónde | Licencia |
|---|---|---|---|---|
| [maxevans60/cm0102-bosman-edition](https://github.com/maxevans60/cm0102-bosman-edition) | `bd78ff0dc8233842cd926244890cbc9143df7390` | base del traductor (`tools/recomp/{analysis,translate}.py` y las utilidades de `recomp.py`) y del runtime (`src/rt/core/*`, `src/rt/win/{kernel32,seh,locale}.c`), modificados para este juego | `tools/recomp/`, `src/rt/` | GPL-3.0 |
| [lieff/minimp3](https://github.com/lieff/minimp3) | `afb604c06bc8beb145fecd42c0ceb5bda8795144` | decodificador MP3 (`minimp3.h`, sin cambios; lo usa `src/rt/win/dshow.c`) | `vendor/minimp3/` | CC0-1.0 |

Modificaciones respecto del original (GPL-3.0 § 5): desde octubre de 2026, para traducir PC Fútbol 2001 en vez del juego
para el que se escribió (tablas de direcciones de ese juego retiradas, lector de PE propio, instrucciones nuevas,
DirectX 7, DirectShow y el resto del runtime de `src/rt/win`).
