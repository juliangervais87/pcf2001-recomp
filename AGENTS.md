# Guía para agentes

Recompilación estática de PC Fútbol 2001: el exe y las DLL del juego traducidos de x86 a C, más un runtime que
imita Windows y DirectX sobre SDL2. El objetivo es que el juego corra igual que en Windows, sin emulador.

## Por dónde empezar

1. [`docs/estado.md`](docs/estado.md): qué anda y los pendientes en orden, con las pistas ya juntadas. El pendiente 0
   (crash al volver del partido al menú) es el primero.
2. [`docs/como-funciona.md`](docs/como-funciona.md): el método, qué imita cada archivo de `src/rt`, las variables de
   entorno, los mensajes de parada y el ciclo de un arreglo.
3. [`docs/ficha-tecnica.md`](docs/ficha-tecnica.md): cómo es el juego por dentro y la tabla de problemas ya resueltos
   (síntoma → causa → archivo). Antes de investigar un síntoma, mirar si ya pasó.
4. [`docs/optimizaciones.md`](docs/optimizaciones.md): optimizaciones recomendadas, sin medir todavía, en orden.
5. [`docs/re/sonido-y-musica.md`](docs/re/sonido-y-musica.md): ejemplo de cómo se documenta un hallazgo, con
   direcciones como evidencia.

## Reglas

- **Nada del juego al repo.** Ni sus archivos, ni el C generado a partir de ellos: todo eso vive en `generated/`, que
  está ignorado. Antes de commitear, revisar que no entre ningún `.exe`, `.dll`, `.pak`, `.dbc` ni nada de `generated/`.
- **Imitar Windows, no parchear el juego.** Un arreglo va en el runtime (`src/rt/win`) o en el traductor
  (`tools/recomp`), nunca en el C generado, que se pisa en cada traducción. Lo que el juego hace raro, primero se
  confirma en el desensamblado; si el original lo hace así, se imita así.
- **Cada hallazgo, con su evidencia.** Dirección del juego, llamada y resultado; distinguir lo verificado de lo
  deducido. Un pendiente resuelto se mueve de `docs/estado.md` a la tabla de la ficha técnica.
- **Verificar sin ventana y después jugando.** La traza (`RECOMP_TRACE=1` o `=2`) dice qué pidió el juego y qué le
  devolvió el runtime. Un arreglo no está terminado hasta que lo que ya andaba sigue andando: menú, pretemporada,
  partido en resultado y en visionado, sonido y música.
- **Memoria.** Traducir pide ~1,4 GB. Compilar con `--jobs 1` es lo seguro en una máquina compartida.
- **Código de terceros.** Lo que se tome de otro proyecto se anota en [`docs/procedencia.md`](docs/procedencia.md) con
  su licencia. El repo es GPL-3.0.

## Comandos

```bash
bash tools/pcf2001_preparar.sh <ISO>      # una vez: genera generated/pcf2001/{cd,instalado,input,c_base}
python tools/recomp/build.py --juego config/juegos/pcf2001.json --input generated/pcf2001/input --out generated/pcf2001/build
python tools/recomp/compile.py --build generated/pcf2001/build --jobs 1
RECOMP_TRACE=2 bash tools/pcf2001_correr.sh 60   # sin ventana, con traza: generated/pcf2001/run/log.txt
bash tools/pcf2001_jugar.sh                      # con ventana
bash tools/pcf2001_web.sh                        # versión web (Emscripten); ?trace=1 en la URL prende la traza
```
