# Ficha técnica de PC Fútbol 2001

## El CD y la instalación

- ISO: 687 MB, etiqueta del volumen `PCF_2001`, creada el 04/12/2000, AppName «PC FUTBOL 2001».
- Instalador InstallShield 6 (`SETUP.EXE` + `DATA1.CAB`/`DATA1.HDR`/`DATA2.CAB`; se extrae con `unshield`). No usa el
  registro de Windows: el juego trabaja relativo a su carpeta (`C:\PCF2001`).
- Lanzador `pcf2001.exe` (40 KB): revisa el CD por `DISK.ID` (contenido `PCF9 00`), pasa el video
  `PLAYVID.EXE <CD>INTROPCF.MPG` y abre `MANAGER.EXE "<ruta del lanzador>"`. También lanza `DBASEWIN.EXE 0..4` (el
  editor de la base de datos). El recompilado arranca directo con esa línea de comandos (perfil `linea_comandos`).
- Sin protección ni empaquetador: Visual C++ normal (entropía ~6,4).
- Lo que copia el instalador: `dat.pak`, `Winfonts.pak`, `SFX.DAT`, `DBDAT`, `Tactics`, `Simuldat`, `CURSORES`, `Ayuda`.
  El resto se lee del CD: gráficos (`GRAFICOS.PAK`), texturas, estadios, y los comentarios del relator
  (`SFX\SIMCOMENT`, 150 MB).

## El programa

- `manager.exe`: 7,8 MB (`.text` 6,3 MB), sin `.reloc`, base 0x400000–0xBB2000, con TLS. C++ con MFC 4.2.
- Casi no le habla a Windows directo: de sus imports, 848 son de `gfxwin.dll`, 86 de MFC, 62 de MSVCRT, 57 de `d2s2.dll`,
  47 de KERNEL32 y sólo 12 de USER32 + GDI32. El juego va arriba de un motor propio de Dinamic.
- DLL propias:
  - `gfxwin.dll` (1,1 MB): ventanas, pantallas, 2D, elección del motor de dibujo;
  - `d2s2.dll`: sonido (`CD2S2SoundManager`, sonido 3D por software, música por DirectShow);
  - `utils.dll`: utilidades (con MMX escrito a mano);
  - `dcd3d.dll`, `DCSF.dll`, `dcsfx.dll`: motores de dibujo;
  - `DllDPlay.dll`, `DLLSockets.dll`: multijugador (DirectPlay y sockets), no traducidas.
- Del sistema, en el CD: MFC42 (1999), MSVCRT, MSVCP60, MSVCIRT, OLEAUT32.
- Tamaño traducido: ~54.700 funciones, 4,5 millones de instrucciones, 99,987 % de cobertura.

## Motores de dibujo

`gfxwin` tiene una tabla en 0xf1e964; se elige con un argumento `<nombre>.DLL` en la línea de comandos:

| # | DLL | Nombre en el juego | Qué usa |
|---|---|---|---|
| 0 | `dcsf.dll` | «Software» | DirectDraw 1/4 + Direct3D 3, 256 colores |
| 1 | `dcsfx.dll` | «Software MMX» (**de fábrica**) | DirectDraw 7 + Direct3D 7, dispositivo RGB (`a4665c60-…`), color alto |
| 2 | `dcd3d.dll` | Direct3D | por hardware |
| 3 | `dcogl.dll` | OpenGL | no viene en el CD |

**Todo el dibujo pasa por Direct3D, menús incluidos.** Los motores «Software» no son un motor propio: usan el
rasterizador por software de Direct3D. Sin un dispositivo Direct3D el juego dice «No se pudo iniciar DirectX» y sale. Si
el motor elegido falla, no prueba otro. Por eso el runtime trae su propio rasterizador (`src/rt/win/d3d.c`).

## Datos

- Base de datos `.DBC` en `DBDAT` (árbitros, historial de ligas, supercopas, jornadas…). Algunas cabeceras traen la
  fecha de su formato: `ARBITROS.DBC` «Copyright (c)1996 Dinamic Multimedia», `HISTORIALIGA`/`SUPERCOPAS` «(c)1999».
- Paquetes gráficos `.PAK` con cabecera `PAK_` y nombres adentro.
- Partido 3D: `SIMULDAT`, `PLAY3D`, `CAMPO`, `3DDXF` (algunos nombres ofuscados, p. ej. `IIJEVOFO.GIO`).
- Sonido: `SFX\GUISND`, `GUISPEECH`, `MUSICAS`, `NGI`, `SIMAMB`, `SIMCANTIC`, `SIMCOMENT`; índice en `SFX.DAT`
  (ver [`re/sonido-y-musica.md`](re/sonido-y-musica.md)).

## Mapa de memoria (perfil)

| Imagen | Base |
|---|---|
| `manager.exe` | 0x400000 |
| MSVCRT | 0xC00000 |
| MSVCP60 | 0xC40000 |
| MFC42 | 0xCB0000 |
| utils | 0xDB0000 |
| gfxwin | 0xE20000 |
| d2s2 | 0xF90000 |
| dcd3d | 0x1010000 |
| DCSF | 0x1030000 |
| dcsfx | 0x1050000 |
| heap | desde 0x1100000 |

Instalación virtual en `C:\PCF2001`; el CD en `D:`. fs:[0] (el TEB) está en la dirección 0 de la memoria.

## Problemas resueltos (síntoma → causa → dónde se arregló)

| Síntoma | Causa | Arreglo |
|---|---|---|
| «No se pudo iniciar DirectX» y sale | todo el dibujo pasa por Direct3D 7 (dispositivo RGB), menús incluidos | rasterizador por software, `src/rt/win/d3d.c`; DirectDraw 4/7 con color alto, `ddraw.c` |
| «jump to … not an entry» en tablas de `memcpy` de MSVCRT | tablas de salto con destinos lejos de la función | barrido de tablas relajado, `build.py` |
| llamadas a direcciones sin traducir apuntadas desde vtables | thunks `jmp [IAT]` del exe | se toman como entradas, `pe_image.py` (`import_thunks`) + `build.py` |
| caída en un manejador de excepción sin traducir | el traductor no reconocía el prólogo SEH de MSVC | las tres formas del prólogo, `build.py` |
| `emms` / instrucciones MMX sin traducir (visionado) | `utils.dll` y `dcsfx.dll` usan MMX | registros en `Cpu.mm`, `translator.py` |
| caída en la pretemporada | faltaban `CharLowerBuffA`, OLE de MFC (`OleInitialize`, `CoGetClassObject`) y el control WebBrowser del panel de ayuda | `user32.c`, `com.c`, `oleaut32.c`, `webbrowser.c` |
| «demasiados objetos COM» | el juego crea miles de superficies | tope alto y reuso de objetos liberados, `com.c` |
| traducir usaba ~10 GB de RAM | el analizador acumula sin límite | dos pasadas con vaciado de cachés, `build.py` |
| el teclado no responde en el partido | el partido mapea las teclas por el código de escaneo del `lParam` | `input.c` (`win_vk_to_scan`), `MapVirtualKeyA` real |
| el juego está mudo | `d2s2` exige DirectMusic para iniciar el sonido | DirectMusic mínimo, `com.c` |
| los efectos no suenan | WAV parseados desde memoria (`FOURCC_MEM`) | mmio en memoria, `winmm.c` |
| fritura en el audio | el mezclador recalculaba la posición en cada callback y leía delante del cursor de escritura | lectura continua y atrasada, `audio.c` |
| pasados ~256 efectos, silencio | un buffer de DirectSound por efecto, nunca liberado | baja y reuso de buffers, `dsound.c` |
| no hay música | la música va por DirectShow con filtros propios del juego | `dshow.c` + minimp3 |
| crash al cerrar (manejador 0x9d30ba sin traducir) | tercera forma del prólogo SEH (`mov eax, fs:[0]; push -1; push manejador`) | `build.py` |
