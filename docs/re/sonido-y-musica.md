# Sonido y música del PC Fútbol 2001 (`d2s2.dll`)

Direcciones con `d2s2` rebasada a 0xF90000 (base original 0x10000000: restar 0xF90000 y sumar 0x10000000 para ir al
binario). **[V]** = verificado en el código o los datos; **[D]** = deducido.

## Arranque del sonido

- **[V]** `d2s2` pide `CoCreateInstance(CLSID_DirectMusic 636b9f10-0c7d-11d1-95b2-0020afdc7421)` y usa `EnumPort`
  (cuenta puertos MIDI) y `SetDirectSound`. Sin DirectMusic el arranque del sonido falla y el juego queda mudo, aunque
  después no lo usa para sonar. Basta un `IDirectMusic`/`IDirectMusic2` mínimo: `EnumPort` → S_FALSE (sin puertos),
  `SetDirectSound` → OK (`src/rt/win/com.c`).
- **[V]** Hace dos pasadas de inicio. La primera es una prueba: crea el primario (65536 B, 22050 Hz estéreo, flags
  0x11) y un buffer 3D mono de 11025 Hz (flags 0xb2), le pide `IKsPropertySet` (31efac30-…; si no hay, sigue) y suelta
  todo. La segunda queda.
- **[V]** `DirectMusicPerformance` (d2ac2881-…) también se pide y falla («clase no registrada»): el juego sigue (MIDI).

## Efectos (DirectSound + mmio)

- **[V]** Los WAV vienen en paquetes (`SFX\GUISND`, `GUISPEECH`…) que `d2s2` lee con `mmioOpen` sobre el archivo y
  también como archivos en memoria (`FOURCC_MEM`: nombre NULL y `MMIOINFO` con `pchBuffer`/`cchBuffer`).
- **[V]** 0xfa2220 abre con `mmioOpen(obj->nombre, NULL, MMIO_WRITE)` y un nombre vacío, y ajusta tamaños de chunk con
  `mmioAscend`. Falla, ~400 veces por minuto, igual que fallaría en Windows: es una rutina de escritura que no hace falta.
- **[V]** Crea **un buffer secundario por cada efecto que suena** (8 o 16 bits, 11025-44100 Hz, flags 0x500e0) y lo
  libera al terminar. El runtime tiene que liberar y reusar los huecos: con un tope fijo de 256 se quedaba mudo.
- **[V]** Le pide `IDirectSound3DBuffer` (279afa86-…) a buffers que no son 3D: falla, como en Windows.

## Música (DirectShow)

La música no usa `RenderFile` ni archivos temporales. `d2s2` arma un FilterGraph con un **filtro fuente propio** y un
**renderer propio**, y DirectShow pone en el medio el splitter y el decodificador MP3.

- **[V]** `CoCreateInstance(CLSID_FilterGraph e436ebb3-…, IID_IFilterGraph 56a8689f-…)` en el método `Open` del
  decodificador DirectShow (0xfaad50, slot +0x20 de la vtable 0xfdafe8), llamado desde 0xfaae3b y 0xfab285. El
  despachador 0xfa1c30 prueba tres decodificadores en orden. El primero intenta abrir la pista como WAV con
  `mmioDescend` y falla, porque es MP3 crudo. El tercero es el de DirectShow.
- Orden de llamadas **[V]**:
  1. QI a IGraphBuilder `56a868a9` y a IMediaControl `56a868b1`.
  2. Crea la fuente (`new(0x170)`, constructor 0xfa9b00) y el renderer (`new(0x124)`, constructor 0xf9b670, vtable 0xfdb62c).
  3. `AddFilter` (+0x0C) de los dos.
  4. `IGraphBuilder::Render` (+0x30) sobre el pin 0 de la fuente.
  5. Revisa que el tipo conectado en el renderer sea `auds`/PCM con `cbFormat == 18` (WAVEFORMATEX) y copia frecuencia,
     canales y bits.
  6. QI a IMediaSeeking `36b73880` y `GetDuration` (+0x28).
  7. `IMediaControl::Pause` (+0x20).
- **[V]** La fuente es un IAsyncReader al estilo del ejemplo «Async» del SDK. Ofrece MEDIATYPE_Stream /
  MPEG1Audio, lee con `fseek(offset+pos)` + `fread` del paquete, y `Length` devuelve el tamaño de la pista.
- **[V]** El renderer acepta sólo `auds`/PCM. Por cada muestra (0xfaba60) agrega los bytes a un buffer propio. Si ya
  juntó lo que el consumidor pidió, o llegó el fin, llama a `Pause` y hace `SetEvent`; si no, `Pause` + `Run`.
  `EndOfStream` (0xfabc00) marca el fin.
- **[V]** El consumidor (0xfac2d0) hace `Run` y espera el evento con `WaitForSingleObject(INFINITE)`. El fin de pista
  es EndOfStream con el buffer vacío. No hay rebobinado ni bucle: la siguiente pista la pide una capa de más arriba.
- La imitación (`src/rt/win/dshow.c`):
  - `Render` lee la pista entera por el IAsyncReader de la fuente, la decodifica con minimp3 y conecta el pin de
    entrada del renderer (`ReceiveConnection`) a un pin de salida PCM propio.
  - `Run` le entrega muestras de 1152 cuadros por `IMemInputPin::Receive`, de forma sincrónica, hasta que el renderer
    pide `Pause` o se acaba la pista (`EndOfStream`).

## El paquete `SFX\Musicas` y el índice `SFX.DAT`

- **[V]** `Musicas` no trae índice: empieza con el magic `EF CD AB 00` (0xABCDEF, chequeado en 0xfc3b55) y siguen los
  MP3 pegados. Son 4 pistas MPEG-1 Layer III crudas, 128 kbps, 44.100 Hz, de unos 2 minutos cada una:

  | Entrada | Pista | Offset | Tamaño |
  |---|---|---|---|
  | 532 | `pcfutok3` | 0x4 | 0x1C6E5F |
  | 533 | `pcfutok2` | 0x1C6E63 | 0x20629D |
  | 534 | `pcfutok1` | 0x3CD100 | 0x1C4F59 |
  | 535 | `pcfutok0` | 0x592059 | 0x1CD53A |

- **[V]** El índice está en `PCF2001\SFX.DAT`. Primero una parte de texto: líneas `id x tipo ruta` más una línea de
  parámetros. Al final, un bloque binario al que apunta el último dword del archivo. El bloque arranca con el magic
  0xABCDEE, 0x30bd, 9 y 0xFEDCBA00, y sigue con pares `(offset, tamaño)` por entrada (lector en 0xfc6429–0xfc6837).
