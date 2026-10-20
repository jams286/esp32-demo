# ESP32 demo

Firmware para explorar lo que un ESP32 puede hacer por sí solo, sin conectarle ningún componente. Todo se controla con una tecla desde el monitor serie, y los resultados se muestran ahí mismo con colores y gráficas de barras en texto.

## Qué incluye

| Tecla | Función |
|---|---|
| `i` | Información del chip: modelo, núcleos, flash, RAM, MAC, versión del ESP-IDF, motivo del último reinicio |
| `n` | Estado de la conexión Wi-Fi |
| `a` | Abrir o cerrar el portal de configuración Wi-Fi |
| `x` | Olvidar la red Wi-Fi guardada |
| `w` | Escaneo de redes Wi-Fi con intensidad de señal y saturación por canal |
| `b` | Escaneo de dispositivos Bluetooth LE cercanos (identifica algunos fabricantes) |
| `c` | Benchmark de números primos: un núcleo contra dos núcleos |
| `t` | Sensores táctiles capacitivos en vivo (basta con tocar los pines con el dedo) |
| `r` | Generador de números aleatorios por hardware, con histograma de 60 000 tiradas de un dado |
| `l` | LED integrado: apagado, parpadeo o respiración por PWM |
| `s` | Deep sleep de 10 s con despertar por temporizador |
| `u` | Coprocesador ULP: lecturas táctiles mientras el CPU principal duerme |
| `h` | Menú |

### Configuración Wi-Fi con portal cautivo

Funciona como muchos dispositivos IoT comerciales:

1. Si no hay una red guardada, o no logra conectarse, el ESP32 crea la red abierta **ESP32-Setup**.
2. Al conectarte desde el celular se abre sola la página de configuración. Si no aparece, entra a `http://192.168.4.1`.
3. Eliges tu red, escribes la contraseña y la página muestra si la conexión funcionó o por qué falló (contraseña incorrecta, red no encontrada, etc.).
4. Las credenciales se guardan en la memoria NVS del ESP32, y solo cuando la conexión fue exitosa. En los siguientes arranques se conecta solo.
5. Ya conectado, hay un panel web en `http://esp32-demo.local` o en la IP que se muestra en el monitor serie.

Las credenciales nunca salen del ESP32 ni se imprimen en el monitor serie (solo se muestra el largo de la contraseña).

### Coprocesador ULP

El ULP FSM del ESP32 se programa con las macros de `esp32/ulp.h`, así que no hace falta el toolchain de ULP. Mientras el CPU está en deep sleep, el ULP:

- despierta cada ~250 ms y hace parpadear el LED (GPIO2 se controla desde el dominio RTC),
- espera a que termine la medición del sensor táctil de GPIO4 y guarda el valor en la RAM RTC,
- tras 60 lecturas despierta al CPU, que dibuja la gráfica junto con el tiempo dormido y el periodo real del ULP.

Al tocar GPIO4 mientras el ESP32 duerme, cada toque se ve como una caída en la gráfica.

Notas técnicas:

- El sensor de temperatura interno (`TSENS`) devuelve un valor fijo en muchos ESP32 clásicos, por eso la demo usa el sensor táctil.
- Para que el sensor táctil mida correctamente en deep sleep hay que mantener encendido el reloj RC_FAST (`ESP_PD_DOMAIN_RC_FAST`). Además, el ULP debe esperar a `SENS_TOUCH_MEAS_DONE` antes de leer.

## Requisitos

- Una placa con **ESP32 clásico** (por ejemplo, ESP32 DevKit V1 o ESP32-WROOM-32). Probado con un ESP32-D0WD-V3 con 4 MB de flash. Las variantes S2, S3, C3 y C6 no son compatibles: el ULP, los registros y los pines táctiles son distintos.
- [PlatformIO](https://platformio.org/), ya sea la extensión de VS Code o la CLI.

La plataforma [pioarduino](https://github.com/pioarduino/platform-espressif32) (Arduino core 3.x) se descarga automáticamente en la primera compilación.

## Uso

```bash
pio run -t upload
pio device monitor
```

PlatformIO detecta el puerto automáticamente. Si tienes varias placas conectadas, indica el puerto con `--upload-port` y `--port`.

Al abrir el monitor, presiona `h` para ver el menú o el botón EN de la placa para ver el arranque completo. Se recomienda una terminal con soporte de UTF-8 y colores ANSI, como Windows Terminal o la terminal de VS Code.

## Licencia

[MIT](LICENSE)
