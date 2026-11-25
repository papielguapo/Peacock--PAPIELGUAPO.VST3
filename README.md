# Peacock

**Peacock** es un efecto VST3 de **PAPIELGUAPO** para Windows x64. Añade color inspirado en la reproducción de vinilo mediante saturación de medios y compresión de agudos. Funciona en pistas mono y estéreo. El DSP y la interfaz son propios: no es una conversión del AAX ni se ha verificado una equivalencia sonora 1:1.

![Interfaz de Peacock con Harmonic en 60, Dynamic en 67, Dither en cero y Color en Fat](docs/peacock.png)

## Instalación

1. Descarga [Peacock para Windows x64](https://github.com/papielguapo/Peacock/releases/download/v0.1.0/Peacock-Windows-x64.zip) y descomprime el ZIP.
2. Copia **la carpeta completa** `Peacock.vst3` en `C:\Program Files\Common Files\VST3`.
3. Vuelve a escanear los plugins en tu DAW e inserta **Peacock** como efecto de audio.

## Controles

| Control | Qué cambia |
| --- | --- |
| **Harmonic** · 0–100 | Satura la banda media y atenúa los agudos en función de su nivel. Sube este control para suavizar un ataque brillante. |
| **Dynamic** · 0–100 | Añade una componente de segundo armónico y modifica su fase según la energía de los agudos. No es un control de volumen ni un compresor de banda completa. |
| **Dither** · 0–100 | Mezcla ruido de bajo nivel con un componente filtrado. En cero no añade ruido; no convierte la salida a 16 bits. |
| **Color** | Silver, Gold, Rich, Fat y Deep cambian la banda y la intensidad de la coloración. Hacia Deep, el efecto alcanza frecuencias más graves. |

**Para escucharlo:** empieza con Harmonic, Dynamic y Dither en cero: la señal pasa sin cambios. En un bucle de batería, selecciona **Fat**, sube **Harmonic** a **60** y después **Dynamic** a **67**, como en la captura. Alterna el bypass del DAW para comparar el ataque de platos y caja. Deja Dither en cero si solo quieres escuchar la coloración.
