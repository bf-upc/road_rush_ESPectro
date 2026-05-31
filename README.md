# 🏎 Road Rush

Juego de carreras para la consola portátil **ESPectro** (ESP32-S3).

> Parte del proyecto ESPectro — [base_espectro](https://github.com/bf-upc/base_espectro)

---

## Descripción

Esquiva obstáculos que aparecen aleatoriamente en la carretera. Cuantos más obstáculos superes, mayor es tu puntuación. La velocidad aumenta al empujar el joystick hacia arriba.

## Controles

| Control | Acción |
|---------|--------|
| Joystick eje X | Mover el coche izquierda/derecha |
| Joystick eje Y (arriba) | Acelerar |
| Botón A | Confirmar / reiniciar tras game over |
| Botón B | Acceder al Game Loader |

## Puntuación

- +1 punto por cada obstáculo superado
- El récord se guarda automáticamente en la memoria flash (NVS)
- El historial de las últimas 20 partidas es visible en el dashboard

## Dashboard

Con la consola encendida, conéctate a la red WiFi **ESPectro** (contraseña: `gameloader`) y abre `http://192.168.4.1` para ver las estadísticas en tiempo real.

## Compilar y flashear

```bash
git clone https://github.com/bf-upc/road_rush_espectro
cd road_rush_espectro
pio run --target upload
```

**Requisitos:**
- PlatformIO
- Librería `lovyan03/LovyanGFX @ ^1.1.12`

El binario compilado se encuentra en `.pio/build/rymcu-esp32-s3-devkitc-1/firmware.bin` y puede subirse vía Game Loader sin cables.