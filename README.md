# MilsToz Mic v2 (VST 2.x, 64 bits, para Equalizer APO)

EQ de 5 bandas + HPF y una cadena de "micrófono profesional" pensada para podcast / streaming.
Latencia 0 muestras, sin memoria dinámica en el hilo de audio, FTZ/DAZ activado (sin denormales),
cada etapa en 0 % se salta por completo (CPU cero).

## Cadena de audio

`HPF (Off | Smooth 12 dB/oct | Cut 24 dB/oct) -> NOISE GATE -> EQ 5 bandas -> CUERPO -> COMPRESSION (Soft|Medium|Hard) -> DE-ESSER -> BRILLO -> MIX -> SALIDA`

| Control      | Qué hace |
|--------------|----------|
| NOISE GATE   | Puerta suave. **0 % = apagado** (valor por defecto). Release interno fijo al 4.5 % de su escala (0-3000 ms = 135 ms) + 80 ms de hold. |
| CUERPO       | Paso-banda 120-220 Hz en paralelo + armónicos pares (saturador asimétrico suave) + low-shelf cálido (hasta +3 dB). |
| COMPRESSION  | Una sola perilla 0-100 % con maquillaje automático. Modos: **Soft** (opto/LA-2A: rodilla 12 dB, ataque 25 ms, release según programa), **Medium** (VCA: rodilla 6 dB, ataque 10 ms, ~4:1), **Hard** (FET/1176 anti-gritos: rodilla rígida, ataque 0.4 ms, hasta 10:1). |
| DE-ESSER     | Detector 6-8 kHz; reduce solo la banda alta (> 5.5 kHz). |
| BRILLO       | De-harsh 3.5-4.8 kHz (centro 4.15 kHz) + excitador de aire > 10 kHz + shelf sedoso a 12 kHz. |
| MIX          | Dry/Wet paralelo 0-100 % (0 = señal original). |
| SALIDA       | Ganancia final + soft-clip transparente (rodilla a -3 dBFS) + limitador brickwall: techo **-0.5 dBFS**. |

## Presets

Barra superior: `<` `>`, lista desplegable (al pasar el ratón se previsualiza su curva), **Guardar Preset** y **Cargar Preset** (`.json`).
El preset activo y todos los ajustes se guardan en el chunk del VST, que es lo que Equalizer APO conserva en su configuración.
De fábrica: Default (Plano: todo apagado, señal original), MilsToz Broadcast (Voz Gruesa), Voz Cálida y Profunda, Voz Cristalina & Aire, Podcast Dinámico / En Vivo, Voz Aguda & Femenina, Control de Gritos (Gamer / Reacción).

## Compilar (elige UNA opción)

1. GitHub Actions: sube el repo -> pestaña Actions -> descarga `MilsTozMic-Plugin`.
2. Visual Studio Build Tools: "x64 Native Tools Command Prompt" -> `build_msvc.bat`.
3. MinGW-w64 (64 bits): `build_mingw.bat`.

Resultado: `build\VoiceEQMic.dll` -> cópialo a `C:\Program Files\EqualizerAPO\VSTPlugins\`
(el nombre del .dll se mantiene para no romper tu configuración actual de Equalizer APO).

Los ajustes guardados con versiones anteriores (VEQ1/VEQ2/VEQ3) se migran solos (se toma el slot que estaba activo; el HPF queda en Smooth si estaba activo).

## Archivos

- `src/dsp.h` motor de audio · `src/params.h` parámetros + presets de fábrica · `src/presets.h` guardado/carga .json
- `src/state.h` estado compartido sin bloqueos · `src/plugin.cpp` plugin VST2 (dispatcher, chunk, migración)
- `src/gui_win32.cpp` / `src/gui.h` interfaz (GDI puro) · `src/vst2_abi.h` ABI mínima VST2
- `tests/` pruebas (`sh tests/run_tests.sh` en Linux/WSL)
