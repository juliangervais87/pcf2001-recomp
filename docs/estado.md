# Estado de PC Fútbol 2001

## Qué anda

- Se juega en Windows con ventana y en pantalla completa estirada (F11 / Alt+Enter):
  - presentación → menú principal → pretemporada → hub (Menú Manager) → selección de equipo;
  - partido en modo **resultado** y en **visionado 3D**;
  - ESC pausa;
  - cambiar a 1024x768 en la pausa.
- **Versión web** (`tools/pcf2001_web.sh`): arranca en el navegador con la pantalla de carga (logo animado de la
  instalación, porcentaje y barra) y llega al menú. El enlace con `-O2` pide ~11,5 GB y ~35 min (ver O9 en
  [`optimizaciones.md`](optimizaciones.md)). En la consola se repite `IDirectSoundBuffer::QueryInterface(31efac30-…): no`:
  es `IKsPropertySet` (EAX), que no hay, como en una placa sin EAX; el juego sigue igual.
- **Sonido completo**: efectos de los botones y las transiciones, relator, ambiente y pelota en el partido, sin fritura.
  La música suena y cambia de canción. Cómo: [`re/sonido-y-musica.md`](re/sonido-y-musica.md).

## Pendientes, en orden sugerido

0. **Crash al volver del partido al menú** (reproducible: jugar un partido y salir al menú).
   - Qué pasa: al salir del partido el juego lanza una excepción C++ a propósito (`0xe06d7363` desde
     `_CxxThrowException` 0xc0ac4e) para desarmar el partido. Un catch la vuelve a lanzar: hay un 2.º y un 3.º
     `RaiseException`, ya con los marcos de MSVCRT 0x10ff3e2c→0xc0ad89 y 0x10ff3e84→0xc0ef03 arriba de la cadena.
     En el tercero, `_except_handler3` (0xc0ef03) recibe un marco 0xabaec0 sin scopetable y salta a basura
     (`call to 0x11000000 from 0xc0ef4e`).
   - Hipótesis: es nuestro `src/rt/win/seh.c`, no el original. Windows trata la excepción anidada (lanzada mientras se
     despacha otra) salteando los marcos que ya se están despachando (ExceptionNestedException); nuestro
     `RaiseException` recorre toda la cadena desde arriba. Falta revisar también `RtlUnwind` con un catch que relanza.
   - La traza ya está: `RECOMP_TRACE=1` imprime cada `RaiseException` con la cadena de manejadores.
   - Ojo con el 0x11000000: es `mem[4]`, el StackBase del TEB. Sale al leer `[0 + 4]`, sea por un scopetable nulo (este
     caso) o al llamar un método de un objeto COM liberado (`com_free` deja la vtable en 0). Como DirectSound libera
     buffers, hay que descartar también un uso después del Release.
1. **Jugar una temporada larga**: guardar y cargar, mercado, finanzas, fin de temporada. Lo que se caiga se arregla con
   la traza (`RECOMP_TRACE=1` o `=2`); ver [`como-funciona.md`](como-funciona.md) § Cómo se depura un crash.
2. **Panel de ayuda vacío**: es un WebBrowser de Internet Explorer que el juego aloja con CHtmlView en cada pantalla.
   El nuestro (`src/rt/win/webbrowser.c`) no dibuja; `Navigate` anota la página (`Ayuda\*.htm`). Idea: dibujar el
   texto del .htm en la ventana hija.
3. **Verificación**: la misma partida grabada, en dos corridas con reloj virtual (`RECOMP_FIXED_CLOCK=1` + guion), tiene
   que dar idéntica.
4. **Web (wasm) y Linux**: la versión web anda: carga, presentación y menú en el navegador (`tools/pcf2001_web.sh`);
   falta jugarla a fondo. Linux sin empezar. Cuidado con dos cosas en la web:
   - el mezclador lee 170 ms detrás (la cola de `audio_pump`);
   - el decodificador de música corre en el hilo del juego: una pista de unos 2 minutos se decodifica en un `Render`.
   
   El rasterizador 3D por software va a ser lo más caro.
5. **Optimizaciones** (audio sin cortes en las cargas, velocidad de la web): filas O1-O9 en
   [`optimizaciones.md`](optimizaciones.md).
6. Menores:
   - Video de intro (`PLAYVID.EXE`, MPEG-1): lo pasa el lanzador, no se traduce.
   - Multijugador (`DllDPlay`, `DLLSockets`, Winsock): fuera por ahora.
   - DirectShow: no hay rebobinado ni bucle (el juego no los pide); el grafo no libera sus objetos COM (pocos).
   - Funciones que sólo se alcanzan por punteros: si aparece un «not a recompiled function» con una dirección del
     juego, buscar qué patrón la usa y agregarlo a `build.py` o a una lista de entradas.

## Cómo se trabaja

- Preparar (una vez): `bash tools/pcf2001_preparar.sh <ISO>`.
- Traducir: `python tools/recomp/build.py --juego config/juegos/pcf2001.json --input generated/pcf2001/input --out generated/pcf2001/build`
  (~6 min, pico ~1,4 GB). Sólo reescribe los archivos que cambian.
- Compilar: `python tools/recomp/compile.py --build generated/pcf2001/build --jobs 1` (incremental; desde cero ~40 min).
- Jugar: `bash tools/pcf2001_jugar.sh` (el disco C: queda en `generated/pcf2001/run/jugar` y se conserva).
- Sin pantalla: `bash tools/pcf2001_correr.sh <segundos>` (log en `generated/pcf2001/run/log.txt`).
- Nunca se commitea nada del original ni de `generated/`.

## Lo que se aprendió (para no re-derivar)

- **Todo se dibuja por Direct3D 7**, menús incluidos: `src/rt/win/d3d.c` es un rasterizador por software, con vertex
  buffers. Detalle en [`ficha-tecnica.md`](ficha-tecnica.md) § Motores de dibujo.
- Imitaciones que hicieron falta:
  - DirectDraw 4/7 con color alto (16/32 bits), z-buffer y superficies liberables;
  - ole32/oleaut32 de MFC (`OleInitialize`, `CoGetClassObject`, BSTR/VARIANT);
  - WebBrowser falso;
  - `CharLowerBuffA`;
  - códigos de escaneo en el `lParam` del teclado (el partido mapea las teclas por ahí);
  - miles de objetos COM con reuso;
  - DirectMusic mínimo;
  - mmio en memoria;
  - DirectSound que libera buffers;
  - DirectShow para la música.
- Arreglos del traductor que pidió el 2001:
  - etiquetas de `loop`;
  - saltos fuera de función por `rt_jump`;
  - tablas de `memcpy` lejanas;
  - thunks `jmp [IAT]`;
  - las tres formas del prólogo SEH de MSVC (`push manejador; mov eax, fs:[0]`, `mov eax, manejador; call __EH_prolog`
    y `mov eax, fs:[0]; push -1; push manejador`);
  - MMX (registros en `Cpu.mm`), `fsincos`, `loope`/`loopne`;
  - traducción en dos pasadas con memoria acotada.
- El lanzador `pcf2001.exe` abre `MANAGER.EXE "<su ruta>"`. Un argumento `<x>.DLL` elige el motor de dibujo; no se usa.
