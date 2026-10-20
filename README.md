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
| `o` | Actualizaciones OTA: ranuras, versiones, estado de la imagen y rollback manual |
| `e` | Cifrado por hardware: AES-128/256 en modo CBC y SHA-256 contra una implementación por software con tablas (con autoverificación FIPS y MB/s de cada lado) |
| `h` | Menú |

### Configuración Wi-Fi con portal cautivo

Funciona como muchos dispositivos IoT comerciales:

1. Si no hay una red guardada, o no logra conectarse, el ESP32 crea la red abierta **ESP32-Setup**.
2. Al conectarte desde el celular se abre sola la página de configuración. Si no aparece, entra a `http://192.168.4.1`.
3. Eliges tu red, escribes la contraseña y la página muestra si la conexión funcionó o por qué falló (contraseña incorrecta, red no encontrada, etc.).
4. Las credenciales se guardan en la memoria NVS del ESP32, y solo cuando la conexión fue exitosa. En los siguientes arranques se conecta solo.
5. Ya conectado, hay un panel web en `http://esp32-demo.local` o en la IP que se muestra en el monitor serie.

Las credenciales nunca salen del ESP32 ni se imprimen en el monitor serie (solo se muestra el largo de la contraseña).

### Actualizaciones OTA con rollback automático

El ESP32 se puede actualizar por Wi-Fi, desde el PC (espota) o desde el navegador, sin cable. La tabla de particiones (`partitions.csv`) tiene dos ranuras de aplicación de 1.94 MB (`ota_0` y `ota_1`): la actualización se escribe en la ranura libre y la anterior queda como respaldo. La NVS conserva su offset, así que las credenciales Wi-Fi guardadas sobreviven.

**Configuración inicial**

1. Copia `secrets.ini.example` como `secrets.ini` y cambia `ota_password` (mínimo 8 caracteres, solo letras y números). El archivo está en `.gitignore` y la compilación falla si la contraseña falta o es muy corta.
2. La primera vez hay que grabar por USB, porque cambia la tabla de particiones: `pio run -e esp32dev -t upload`.

**Desde el PC**

```bash
pio run -e esp32dev-ota -t upload
```

Usa `esp32-demo.local` como destino. Si tu red no resuelve mDNS, indica la IP: `pio run -e esp32dev-ota -t upload --upload-port 192.168.1.50`. Con una contraseña incorrecta el ESP32 rechaza la actualización.

En Windows, espota necesita que el firewall permita las conexiones entrantes a `python.exe` (el ESP32 se conecta de vuelta al PC para pedir el archivo). Si la actualización se queda en "Waiting for device", revisa esa regla.

**Desde el navegador**

Abre `http://esp32-demo.local/update` (o el enlace "Actualizar firmware" del panel), entra con usuario `admin` y la contraseña de `secrets.ini`, y sube el archivo `.pio/build/esp32dev/firmware.bin`. Sin credenciales responde 401.

**Rollback automático**

Una imagen recién actualizada arranca "pendiente de verificar". Se valida sola cuando lleva 30 s estable y, si hay una red Wi-Fi guardada, conectada. Si pasan 90 s sin validarse, o si el ESP32 se reinicia o se cae antes, vuelve a la versión anterior y lo avisa en el monitor serie. Mientras está pendiente, el deep sleep (`s`) y el ULP (`u`) se niegan a correr, porque terminan en un reinicio.

Para probarlo, sube la imagen defectuosa: `pio run -e esp32dev-ota-faulty -t upload`. Se cae a los ~5 s de arrancar y el ESP32 regresa a la versión anterior. (Solo se cae si llegó por OTA; grabada por USB funciona normal.)

La tecla `o` muestra la ranura en uso, la otra ranura, las versiones, el estado de la imagen y el último rollback, y con `v` permite volver a la versión anterior a mano.

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
