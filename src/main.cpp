// Demo de lo que el ESP32 puede hacer solo, sin nada conectado.
// Todo se controla con una tecla desde el monitor serie (115200 baudios).
//
// Wi-Fi como un dispositivo IoT: si no tiene red guardada (o no logra conectarse)
// abre la red "ESP32-Setup" con un portal cautivo. Te conectas desde el celular,
// eliges tu red, escribes la contraseña y, si conecta, la guarda en la flash (NVS).

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include <Preferences.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <esp_chip_info.h>
#include <esp_mac.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <sys/time.h>
#include <driver/rtc_io.h>
#include <soc/rtc_cntl_reg.h>
#include <soc/rtc_io_reg.h>
#include <soc/sens_reg.h>
#include <soc/soc.h>
#include <ulp_common.h>
#include <esp32/ulp.h>
#include <algorithm>
#include <vector>

#define LED_PIN 2

#define FW_VERSION "1.1.0"
#define FW_BUILD __DATE__ " " __TIME__

// La contraseña viene de secrets.ini (ver README); sin ella no se compila
#ifndef OTA_PASSWORD
#error "Falta OTA_PASSWORD: crea secrets.ini a partir de secrets.ini.example"
#endif
static_assert(sizeof(OTA_PASSWORD) > 8, "OTA_PASSWORD debe tener al menos 8 caracteres");

#define C_RESET   "\033[0m"
#define C_BOLD    "\033[1m"
#define C_DIM     "\033[2m"
#define C_RED     "\033[31m"
#define C_GREEN   "\033[32m"
#define C_YELLOW  "\033[33m"
#define C_MAGENTA "\033[35m"
#define C_CYAN    "\033[36m"

const char *PORTAL_SSID = "ESP32-Setup";
const char *HOSTNAME = "esp32-demo";
const uint32_t CONNECT_TIMEOUT_MS = 20000;
const uint32_t PORTAL_LINGER_MS = 60000;  // tiempo que sigue el portal tras conectar

// Vive en la RAM del RTC: sobrevive al deep sleep, se borra al quitar la corriente
RTC_DATA_ATTR uint32_t bootCount = 0;
RTC_DATA_ATTR bool ulpPending = false;  // el último deep sleep fue la demo del ULP

WebServer server(80);
DNSServer dns;
Preferences prefs;
bool serverStarted = false;
bool portalActive = false;
bool mdnsStarted = false;
bool bleReady = false;
uint32_t portalCloseAt = 0;  // 0 = sin cierre programado

enum ConnState { CONN_IDLE, CONN_CONNECTING, CONN_OK, CONN_FAILED };
ConnState connState = CONN_IDLE;
String connSsid, connPass, connMsg;
bool connFromPortal = false;
uint32_t connStartedAt = 0;
volatile uint8_t lastDisconnectReason = 0;
bool linkUp = false;  // para avisar cuando se pierde o se recupera la conexión

struct NetInfo {
  String ssid;
  int rssi;
  bool open;
};
std::vector<NetInfo> portalNets;

enum LedMode { LED_OFF, LED_BLINK, LED_BREATHE };
LedMode ledMode = LED_OFF;
const char *ledModeNames[] = {"apagado", "parpadeo", "respiración (PWM)"};

// Pines táctiles capacitivos utilizables (se omiten GPIO0 por arranque y GPIO2 por el LED)
const uint8_t touchPins[] = {4, 15, 13, 12, 14, 27, 33, 32};
const int touchCount = sizeof(touchPins) / sizeof(touchPins[0]);

// ---------- utilidades ----------

void header(const char *title) {
  Serial.printf("\n" C_BOLD C_CYAN "== %s ==" C_RESET "\n", title);
}

void bar(int value, int maxValue, int width, const char *color) {
  int n = constrain(map(value, 0, maxValue, 0, width), 0, width);
  Serial.print(color);
  for (int i = 0; i < n; i++) Serial.print("█");
  Serial.print(C_DIM);
  for (int i = n; i < width; i++) Serial.print("░");
  Serial.print(C_RESET);
}

const char *uptimeStr() {
  static char buf[32];
  uint32_t s = millis() / 1000;
  snprintf(buf, sizeof(buf), "%luh %02lum %02lus", (unsigned long)(s / 3600),
           (unsigned long)(s / 60 % 60), (unsigned long)(s % 60));
  return buf;
}

const char *resetReasonStr(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "encendido / reset por USB";
    case ESP_RST_EXT:       return "pin externo";
    case ESP_RST_SW:        return "reinicio por software";
    case ESP_RST_PANIC:     return "panic (excepción)";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:       return "watchdog";
    case ESP_RST_DEEPSLEEP: return "despertó de deep sleep";
    case ESP_RST_BROWNOUT:  return "brownout (bajo voltaje)";
    default:                return "otro";
  }
}

void flushInput() {
  delay(20);
  while (Serial.available()) Serial.read();
}

String htmlEscape(const String &s) {
  String out;
  out.reserve(s.length() + 8);
  for (char c : s) {
    switch (c) {
      case '&':  out += "&amp;"; break;
      case '<':  out += "&lt;"; break;
      case '>':  out += "&gt;"; break;
      case '"':  out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default:   out += c;
    }
  }
  return out;
}

// ---------- menú ----------

void printMenu() {
  header("ESP32 demo: menú");
  Serial.println("  " C_YELLOW "i" C_RESET "  información del chip");
  Serial.println("  " C_YELLOW "n" C_RESET "  estado de la red Wi-Fi");
  Serial.printf ("  " C_YELLOW "a" C_RESET "  %s el portal de configuración Wi-Fi (%s)\n", portalActive ? "cerrar" : "abrir", PORTAL_SSID);
  Serial.println("  " C_YELLOW "x" C_RESET "  olvidar la red Wi-Fi guardada");
  Serial.println("  " C_YELLOW "w" C_RESET "  escanear redes Wi-Fi (y saturación por canal)");
  Serial.println("  " C_YELLOW "b" C_RESET "  escanear dispositivos Bluetooth LE cercanos");
  Serial.println("  " C_YELLOW "c" C_RESET "  benchmark: 1 núcleo contra 2 núcleos");
  Serial.println("  " C_YELLOW "t" C_RESET "  sensores táctiles en vivo (toca los pines con el dedo)");
  Serial.println("  " C_YELLOW "r" C_RESET "  generador de números aleatorios por hardware");
  Serial.printf ("  " C_YELLOW "l" C_RESET "  cambiar modo del LED azul (ahora: %s)\n", ledModeNames[ledMode]);
  Serial.println("  " C_YELLOW "s" C_RESET "  deep sleep 10 s y despertar solo");
  Serial.println("  " C_YELLOW "u" C_RESET "  coprocesador ULP: LED y sensor táctil mientras el CPU duerme");
  Serial.println("  " C_YELLOW "o" C_RESET "  actualizaciones OTA: versión, particiones y rollback");
  Serial.println("  " C_YELLOW "h" C_RESET "  este menú");
}

// ---------- información ----------

void printInfo() {
  esp_chip_info_t chip;
  esp_chip_info(&chip);
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);

  header("Información del chip");
  Serial.printf("  Modelo       : %s rev v%d.%d\n", ESP.getChipModel(), chip.revision / 100, chip.revision % 100);
  Serial.printf("  Núcleos      : %d a %lu MHz\n", chip.cores, (unsigned long)getCpuFrequencyMhz());
  Serial.printf("  Radio        : %s%s%s\n",
                (chip.features & CHIP_FEATURE_WIFI_BGN) ? "Wi-Fi b/g/n  " : "",
                (chip.features & CHIP_FEATURE_BT) ? "Bluetooth Classic  " : "",
                (chip.features & CHIP_FEATURE_BLE) ? "BLE" : "");
  Serial.printf("  Flash        : %lu MB a %lu MHz\n", (unsigned long)(ESP.getFlashChipSize() / (1024 * 1024)),
                (unsigned long)(ESP.getFlashChipSpeed() / 1000000));
  Serial.printf("  PSRAM        : %s\n", psramFound() ? "sí" : "no tiene");
  Serial.printf("  RAM libre    : %lu KB de %lu KB (mínimo histórico %lu KB)\n",
                (unsigned long)(ESP.getFreeHeap() / 1024), (unsigned long)(ESP.getHeapSize() / 1024),
                (unsigned long)(ESP.getMinFreeHeap() / 1024));
  Serial.printf("  Este programa: %lu KB (quedan %lu KB)\n", (unsigned long)(ESP.getSketchSize() / 1024),
                (unsigned long)(ESP.getFreeSketchSpace() / 1024));
  Serial.printf("  MAC Wi-Fi    : %02X:%02X:%02X:%02X:%02X:%02X\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  Serial.printf("  ESP-IDF      : %s\n", ESP.getSdkVersion());
  Serial.printf("  Arranques    : %lu (contador en RAM del RTC)\n", (unsigned long)bootCount);
  Serial.printf("  Último reset : %s\n", resetReasonStr(esp_reset_reason()));
  Serial.printf("  Encendido    : %s\n", uptimeStr());
}

// ---------- credenciales y conexión a tu Wi-Fi ----------

bool loadCredentials(String &ssid, String &pass) {
  prefs.begin("wifi", true);
  ssid = prefs.getString("ssid", "");
  pass = prefs.getString("pass", "");
  prefs.end();
  return !ssid.isEmpty();
}

void saveCredentials(const String &ssid, const String &pass) {
  prefs.begin("wifi", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();
}

void forgetCredentials() {
  prefs.begin("wifi", false);
  prefs.clear();
  prefs.end();
}

// Traduce los códigos de desconexión más comunes del ESP-IDF
const char *reasonStr(uint8_t r) {
  switch (r) {
    case WIFI_REASON_NO_AP_FOUND:
      return "no encontré esa red (¿el nombre es correcto? ¿es de 2.4 GHz?)";
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
      return "contraseña incorrecta";
    case WIFI_REASON_ASSOC_FAIL:
      return "el router rechazó la conexión";
    case WIFI_REASON_BEACON_TIMEOUT:
      return "se perdió la señal del router";
    case 210:
    case 211:
      return "la red usa una seguridad que no es compatible (WPA2 o superior)";
    default:
      return nullptr;
  }
}

String describeReason(uint8_t r) {
  const char *why = reasonStr(r);
  if (why) return String(why);
  if (r) return "error " + String(r);
  return String("tiempo de espera agotado");
}

void ensureServer() {
  if (!serverStarted) {
    server.begin();
    serverStarted = true;
  }
}

void startConnect(const String &ssid, const String &pass, bool fromPortal) {
  connSsid = ssid;
  connPass = pass;
  connFromPortal = fromPortal;
  connMsg = "";
  connState = CONN_CONNECTING;
  WiFi.enableSTA(true);
  WiFi.setAutoReconnect(false);  // un fallo debe terminar el intento, no reintentar sin fin
  WiFi.disconnect();
  lastDisconnectReason = 0;
  connStartedAt = millis();
  WiFi.begin(ssid.c_str(), pass.c_str());
  Serial.printf(C_CYAN "  [wifi] conectando a \"%s\"..." C_RESET "\n", ssid.c_str());
}

void startPortal();
void startOta();

void updateConnection() {
  bool up = WiFi.status() == WL_CONNECTED;

  if (connState == CONN_CONNECTING) {
    if (up) {
      connState = CONN_OK;
      linkUp = true;
      WiFi.setAutoReconnect(true);
      String ip = WiFi.localIP().toString();
      Serial.printf(C_GREEN C_BOLD "  [wifi] conectado a \"%s\"  IP %s  (%d dBm)" C_RESET "\n",
                    connSsid.c_str(), ip.c_str(), WiFi.RSSI());
      if (connFromPortal) {
        saveCredentials(connSsid, connPass);
        Serial.println(C_GREEN "  [wifi] credenciales guardadas en la flash (NVS)" C_RESET);
        portalCloseAt = millis() + PORTAL_LINGER_MS;
        Serial.printf(C_DIM "  [portal] la red %s se apagará en %lu s" C_RESET "\n", PORTAL_SSID,
                      (unsigned long)(PORTAL_LINGER_MS / 1000));
      }
      ensureServer();
      if (!mdnsStarted && MDNS.begin(HOSTNAME)) {
        MDNS.addService("http", "tcp", 80);
        mdnsStarted = true;
      }
      startOta();
      Serial.printf("  Desde tu red abre " C_BOLD "http://%s" C_RESET " o " C_BOLD "http://%s.local" C_RESET "\n",
                    ip.c_str(), HOSTNAME);
    } else if (lastDisconnectReason != 0 || millis() - connStartedAt > CONNECT_TIMEOUT_MS) {
      connState = CONN_FAILED;
      connMsg = describeReason(lastDisconnectReason);
      WiFi.disconnect();
      Serial.printf(C_RED "  [wifi] no pude conectar a \"%s\": %s" C_RESET "\n", connSsid.c_str(), connMsg.c_str());
      if (!connFromPortal && !portalActive) startPortal();
    }
    return;
  }

  if (connState == CONN_OK && up != linkUp) {
    linkUp = up;
    if (up)
      Serial.printf(C_GREEN "  [wifi] reconectado, IP %s" C_RESET "\n", WiFi.localIP().toString().c_str());
    else
      Serial.printf(C_YELLOW "  [wifi] se perdió la conexión (%s), reintentando..." C_RESET "\n",
                    describeReason(lastDisconnectReason).c_str());
  }
}

void printNetwork() {
  header("Estado de la red");
  String ssid, pass;
  bool saved = loadCredentials(ssid, pass);
  Serial.printf("  Red guardada : %s\n", saved ? ("\"" + ssid + "\"").c_str() : "ninguna");
  if (WiFi.status() == WL_CONNECTED) {
    int rssi = WiFi.RSSI();
    Serial.printf("  Estado       : " C_GREEN "conectado a \"%s\"" C_RESET "\n", WiFi.SSID().c_str());
    Serial.printf("  IP           : %s  (puerta de enlace %s, DNS %s)\n", WiFi.localIP().toString().c_str(),
                  WiFi.gatewayIP().toString().c_str(), WiFi.dnsIP().toString().c_str());
    Serial.printf("  Nombre       : http://%s.local\n", HOSTNAME);
    Serial.printf("  Señal        : ");
    bar(rssi + 100, 70, 15, rssi > -60 ? C_GREEN : rssi > -75 ? C_YELLOW : C_RED);
    Serial.printf(" %d dBm, canal %ld\n", rssi, (long)WiFi.channel());
  } else {
    const char *st = connState == CONN_CONNECTING ? "conectando..." : connState == CONN_FAILED ? "falló el último intento" : "desconectado";
    Serial.printf("  Estado       : %s\n", st);
    if (connState == CONN_FAILED) Serial.printf("  Motivo       : %s\n", connMsg.c_str());
  }
  if (portalActive)
    Serial.printf("  Portal       : abierto (%s, %d clientes, http://%s)\n", PORTAL_SSID, WiFi.softAPgetStationNum(),
                  WiFi.softAPIP().toString().c_str());
  else
    Serial.println("  Portal       : cerrado");
}

// ---------- portal cautivo ----------

void scanForPortal() {
  portalNets.clear();
  int n = WiFi.scanNetworks();
  for (int i = 0; i < n; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid.isEmpty()) continue;
    int rssi = WiFi.RSSI(i);
    bool found = false;
    for (auto &net : portalNets) {  // varias antenas con el mismo nombre: quedarse con la más fuerte
      if (net.ssid == ssid) {
        net.rssi = max(net.rssi, rssi);
        found = true;
        break;
      }
    }
    if (!found) portalNets.push_back({ssid, rssi, WiFi.encryptionType(i) == WIFI_AUTH_OPEN});
  }
  WiFi.scanDelete();
  std::sort(portalNets.begin(), portalNets.end(), [](const NetInfo &a, const NetInfo &b) { return a.rssi > b.rssi; });
  if (portalNets.size() > 20) portalNets.resize(20);
  Serial.printf(C_DIM "  [portal] %d redes encontradas" C_RESET "\n", (int)portalNets.size());
}

void startPortal() {
  header("Portal de configuración Wi-Fi");
  WiFi.enableSTA(true);
  scanForPortal();  // antes de abrir el AP, para no cortar a nadie
  WiFi.enableAP(true);
  WiFi.softAP(PORTAL_SSID);
  delay(100);
  dns.setErrorReplyCode(DNSReplyCode::NoError);
  dns.start(53, "*", WiFi.softAPIP());  // cualquier dominio apunta al ESP32: así el celular abre el portal solo
  ensureServer();
  portalActive = true;
  portalCloseAt = 0;
  Serial.printf("  1. Desde el celular conéctate a la red " C_BOLD "%s" C_RESET " (sin contraseña).\n", PORTAL_SSID);
  Serial.printf("  2. Debería abrirse sola la página; si no, abre " C_BOLD "http://%s" C_RESET "\n",
                WiFi.softAPIP().toString().c_str());
  Serial.println("  3. Elige tu red y escribe la contraseña.");
  Serial.println(C_DIM "  Si el celular avisa que la red no tiene internet, dile que siga conectado." C_RESET);
}

void stopPortal() {
  dns.stop();
  WiFi.softAPdisconnect(true);
  portalActive = false;
  portalCloseAt = 0;
  Serial.printf(C_DIM "  [portal] red %s apagada" C_RESET "\n", PORTAL_SSID);
}

const char PAGE_HEAD[] PROGMEM = R"html(<!doctype html><html><head><meta charset='utf-8'>
<meta name='viewport' content='width=device-width,initial-scale=1'><title>ESP32</title><style>
*{box-sizing:border-box}body{font-family:system-ui,sans-serif;background:#0f172a;color:#e2e8f0;margin:0;padding:20px}
.card{max-width:420px;margin:auto;background:#1e293b;border-radius:14px;padding:20px}
h1{font-size:1.3em;margin:0 0 4px}.sub{color:#94a3b8;margin:0 0 16px;font-size:.9em}
.net{display:flex;justify-content:space-between;width:100%;padding:12px;margin:6px 0;background:#334155;
border:2px solid transparent;border-radius:10px;color:inherit;font-size:1em;cursor:pointer;text-align:left}
.net.sel{border-color:#3b82f6}.sig{color:#94a3b8;font-size:.85em;white-space:nowrap;margin-left:8px}
label{display:block;margin:14px 0 6px;font-size:.9em;color:#94a3b8}
input[type=text],input[type=password]{width:100%;padding:12px;border-radius:10px;border:1px solid #475569;
background:#0f172a;color:inherit;font-size:1em}
.btn{display:block;width:100%;margin-top:18px;padding:14px;border:0;border-radius:10px;background:#3b82f6;
color:#fff;font-size:1em;font-weight:600;text-align:center;text-decoration:none}
.link{color:#93c5fd;font-size:.9em}.ok{color:#4ade80}.err{color:#f87171}.small{font-size:.85em;color:#94a3b8}
.box{background:#450a0a;border-radius:10px;padding:10px 12px;margin-bottom:12px}
td{padding:6px 10px 6px 0}
</style></head><body><div class='card'>)html";

const char PAGE_TAIL[] PROGMEM = "</div></body></html>";

void sendPage(const String &body, int code = 200) {
  String html;
  html.reserve(strlen_P(PAGE_HEAD) + body.length() + 32);
  html += FPSTR(PAGE_HEAD);
  html += body;
  html += FPSTR(PAGE_TAIL);
  server.send(code, "text/html", html);
}

void handlePortalPage() {
  Serial.printf(C_MAGENTA "  [portal] %s abrió el portal" C_RESET "\n", server.client().remoteIP().toString().c_str());
  String b = "<h1>Conecta el ESP32 a tu Wi-Fi</h1><p class='sub'>Solo ve redes de 2.4 GHz.</p>";
  if (connState == CONN_FAILED && connFromPortal)
    b += "<div class='box err'>No se pudo conectar a <b>" + htmlEscape(connSsid) + "</b>: " + htmlEscape(connMsg) + "</div>";
  if (portalNets.empty()) b += "<p class='small'>No encontré redes. Puedes escribir el nombre a mano.</p>";
  for (auto &net : portalNets) {
    String name = htmlEscape(net.ssid);
    int bars = net.rssi > -55 ? 4 : net.rssi > -67 ? 3 : net.rssi > -78 ? 2 : 1;
    b += "<button type='button' class='net' data-ssid='" + name + "' onclick='pick(this)'><span>" + name +
         (net.open ? "" : " 🔒") + "</span><span class='sig'>" + String("▂▄▆█").substring(0, bars * 3) + " " +
         String(net.rssi) + " dBm</span></button>";
  }
  b += "<form method='post' action='/save'>"
       "<label>Red</label><input type='text' name='ssid' id='ssid' maxlength='32' required>"
       "<label>Contraseña</label><input type='password' name='pass' id='pass' maxlength='63'>"
       "<label class='small'><input type='checkbox' onclick=\"pass.type=this.checked?'text':'password'\"> mostrar contraseña</label>"
       "<button class='btn'>Conectar</button></form>"
       "<p><a class='link' href='/scan'>↻ Buscar redes otra vez</a></p>"
       "<script>function pick(b){document.querySelectorAll('.net').forEach(e=>e.classList.remove('sel'));"
       "b.classList.add('sel');ssid.value=b.dataset.ssid;pass.focus()}</script>";
  sendPage(b);
}

void handleScan() {
  scanForPortal();
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleSave() {
  String ssid = server.arg("ssid");
  String pass = server.arg("pass");
  if (ssid.isEmpty() || ssid.length() > 32 || (pass.length() > 0 && pass.length() < 8) || pass.length() > 63) {
    sendPage("<h1>Datos no válidos</h1><p class='err'>El nombre debe tener hasta 32 caracteres y la contraseña "
             "entre 8 y 63 (o vacía si la red es abierta).</p><a class='btn' href='/'>Volver</a>", 400);
    return;
  }
  // Nunca se imprime la contraseña, solo su largo
  Serial.printf(C_MAGENTA "  [portal] recibí la red \"%s\" (contraseña de %d caracteres)" C_RESET "\n",
                ssid.c_str(), pass.length());
  sendPage("<h1>Conectando a <b>" + htmlEscape(ssid) + "</b>…</h1>"
           "<p id='st' class='sub'>Esto tarda unos segundos.</p><div id='out'></div>"
           "<script>let fails=0;async function poll(){try{const r=await fetch('/status',{cache:'no-store'});"
           "const s=await r.json();fails=0;"
           "if(s.state=='ok'){st.innerHTML=\"<span class='ok'>✔ ¡Conectado!</span>\";"
           "out.innerHTML=\"<p>IP en tu red: <b>\"+s.ip+\"</b></p><p>Vuelve a tu Wi-Fi de siempre y abre:</p>"
           "<a class='btn' href='http://\"+s.ip+\"/panel'>http://\"+s.ip+\"</a>"
           "<p class='small'>o http://esp32-demo.local. La red ESP32-Setup se apagará en un minuto.</p>\";return}"
           "if(s.state=='fail'){st.innerHTML=\"<span class='err'>✖ No se pudo conectar: \"+s.msg+\"</span>\";"
           "out.innerHTML=\"<a class='btn' href='/'>Intentar de nuevo</a>\";return}"
           "}catch(e){if(++fails==5){st.innerHTML='Se perdió la conexión con el ESP32.';"
           "out.innerHTML=\"<p class='small'>Suele pasar justo cuando se conecta a tu router: cambia de canal y el "
           "celular se desconecta un momento. Vuelve a conectarte a ESP32-Setup y abre "
           "<a class='link' href='/'>192.168.4.1</a>, o revisa el monitor serie.</p>\"}}"
           "setTimeout(poll,1000)}poll()</script>");
  startConnect(ssid, pass, true);
}

void handleStatus() {
  const char *st = connState == CONN_CONNECTING ? "connecting" : connState == CONN_OK ? "ok"
                 : connState == CONN_FAILED ? "fail" : "idle";
  String json = "{\"state\":\"" + String(st) + "\",\"ip\":\"" + WiFi.localIP().toString() + "\",\"msg\":\"" + connMsg + "\"}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
}

// Los celulares prueban URLs como /generate_204 o /hotspot-detect.html para saber si hay
// internet. Redirigir todo al portal hace que el sistema muestre la página de inicio de sesión.
void handleNotFound() {
  if (portalActive) {
    server.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/");
    server.send(302);
  } else {
    server.send(404, "text/plain", "No encontrado");
  }
}

// ---------- panel web (desde tu red o desde el portal) ----------

void handlePanel() {
  Serial.printf(C_MAGENTA "  [web] %s abrió el panel" C_RESET "\n", server.client().remoteIP().toString().c_str());
  String b = "<meta http-equiv='refresh' content='3'><h1>Hola desde el ESP32</h1><p class='sub'>Se actualiza cada 3 s</p><table>";
  b += "<tr><td>Encendido</td><td>" + String(uptimeStr()) + "</td></tr>";
  b += "<tr><td>RAM libre</td><td>" + String(ESP.getFreeHeap() / 1024) + " KB</td></tr>";
  if (WiFi.status() == WL_CONNECTED) {
    b += "<tr><td>Red</td><td>" + htmlEscape(WiFi.SSID()) + "</td></tr>";
    b += "<tr><td>IP</td><td>" + WiFi.localIP().toString() + "</td></tr>";
    b += "<tr><td>Señal</td><td>" + String(WiFi.RSSI()) + " dBm</td></tr>";
  }
  b += "<tr><td>LED</td><td>" + String(ledModeNames[ledMode]) + "</td></tr>";
  b += "<tr><td>Arranques</td><td>" + String(bootCount) + "</td></tr>";
  b += "<tr><td>Firmware</td><td>" FW_VERSION "</td></tr>";
  b += "</table><a class='btn' href='/led'>Cambiar LED</a><a class='btn' href='/update'>Actualizar firmware</a>";
  sendPage(b);
}

void handleRoot() {
  if (portalActive) handlePortalPage();
  else handlePanel();
}

// ---------- Wi-Fi: escaneo ----------

const char *authStr(wifi_auth_mode_t a) {
  switch (a) {
    case WIFI_AUTH_OPEN:            return C_RED "abierta" C_RESET;
    case WIFI_AUTH_WEP:             return C_RED "WEP" C_RESET;
    case WIFI_AUTH_WPA_PSK:         return "WPA";
    case WIFI_AUTH_WPA2_PSK:        return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/WPA2";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-Enterprise";
    case WIFI_AUTH_WPA3_PSK:        return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/WPA3";
    default:                        return "otra";
  }
}

void scanWifi() {
  header("Redes Wi-Fi");
  Serial.println(C_DIM "  escaneando..." C_RESET);
  bool staWasOn = WiFi.getMode() & WIFI_STA;
  if (!staWasOn) WiFi.enableSTA(true);
  int n = WiFi.scanNetworks();
  if (n <= 0) {
    Serial.println("  No encontré redes.");
  } else {
    n = min(n, 40);
    int idx[40];
    for (int i = 0; i < n; i++) idx[i] = i;
    for (int i = 1; i < n; i++)  // ordenar por señal, la más fuerte primero
      for (int j = i; j > 0 && WiFi.RSSI(idx[j]) > WiFi.RSSI(idx[j - 1]); j--) std::swap(idx[j], idx[j - 1]);

    int perChannel[15] = {0};
    for (int k = 0; k < n; k++) {
      int i = idx[k];
      int rssi = WiFi.RSSI(i);
      String ssid = WiFi.SSID(i);
      const char *color = rssi > -60 ? C_GREEN : rssi > -75 ? C_YELLOW : C_RED;
      Serial.printf("  %2d) %-28.28s ", k + 1, ssid.isEmpty() ? "(oculta)" : ssid.c_str());
      bar(rssi + 100, 70, 15, color);
      Serial.printf(" %4d dBm  canal %2ld  %s\n", rssi, (long)WiFi.channel(i), authStr(WiFi.encryptionType(i)));
      int ch = WiFi.channel(i);
      if (ch >= 1 && ch <= 14) perChannel[ch]++;
    }

    int maxCh = 1;
    for (int c = 1; c <= 13; c++) maxCh = max(maxCh, perChannel[c]);
    Serial.println("\n  Redes por canal (2.4 GHz):");
    for (int c = 1; c <= 13; c++) {
      Serial.printf("   canal %2d ", c);
      bar(perChannel[c], maxCh, 20, perChannel[c] == maxCh ? C_RED : C_CYAN);
      Serial.printf(" %d\n", perChannel[c]);
    }
  }
  WiFi.scanDelete();
  if (!staWasOn) WiFi.enableSTA(false);
}

// ---------- Bluetooth LE ----------

const char *vendorStr(uint16_t id) {
  switch (id) {
    case 0x004C: return "Apple";
    case 0x0006: return "Microsoft";
    case 0x0075: return "Samsung";
    case 0x00E0: return "Google";
    case 0x0059: return "Nordic";
    case 0x02E5: return "Espressif";
    case 0x0157: return "Huami / Xiaomi";
    case 0x038F: return "Xiaomi";
    default:     return nullptr;
  }
}

void scanBle() {
  header("Dispositivos Bluetooth LE");
  if (!bleReady) {
    BLEDevice::init("");
    bleReady = true;
  }
  BLEScan *scan = BLEDevice::getScan();
  scan->setActiveScan(true);
  scan->setInterval(100);
  scan->setWindow(99);
  Serial.println(C_DIM "  escaneando 5 s..." C_RESET);
  BLEScanResults *found = scan->start(5, false);
  int n = found->getCount();
  if (n == 0) Serial.println("  No encontré dispositivos.");
  for (int i = 0; i < n; i++) {
    BLEAdvertisedDevice d = found->getDevice(i);
    String name = d.haveName() ? String(d.getName().c_str()) : String("(sin nombre)");
    const char *vendor = nullptr;
    if (d.haveManufacturerData()) {
      auto data = d.getManufacturerData();
      if (data.length() >= 2) vendor = vendorStr((uint8_t)data[0] | ((uint8_t)data[1] << 8));
    }
    int rssi = d.getRSSI();
    const char *color = rssi > -60 ? C_GREEN : rssi > -80 ? C_YELLOW : C_RED;
    Serial.printf("  %2d) %s  ", i + 1, d.getAddress().toString().c_str());
    bar(rssi + 100, 70, 12, color);
    Serial.printf(" %4d dBm  %-24.24s %s\n", rssi, name.c_str(), vendor ? vendor : "");
  }
  Serial.printf("  Total: %d dispositivos\n", n);
  scan->clearResults();
}

// ---------- benchmark de doble núcleo ----------

const uint32_t PRIME_LIMIT = 600000;
const uint32_t PRIME_BLOCK = 2000;

struct PrimeJob {
  int part;  // -1 = todo, 0 o 1 = bloques alternos
  uint32_t count;
  TaskHandle_t notify;
};

bool isPrime(uint32_t n) {
  if (n < 2) return false;
  if (n % 2 == 0) return n == 2;
  for (uint32_t d = 3; d * d <= n; d += 2)
    if (n % d == 0) return false;
  return true;
}

uint32_t countPrimes(int part) {
  uint32_t count = 0;
  for (uint32_t b = 0; b * PRIME_BLOCK < PRIME_LIMIT; b++) {
    if (part >= 0 && (int)(b % 2) != part) continue;
    uint32_t end = min((b + 1) * PRIME_BLOCK, PRIME_LIMIT);
    for (uint32_t n = b * PRIME_BLOCK; n < end; n++)
      if (isPrime(n)) count++;
    vTaskDelay(1);  // ceder la CPU para que no salte el watchdog
  }
  return count;
}

void primeTask(void *arg) {
  PrimeJob *job = (PrimeJob *)arg;
  job->count = countPrimes(job->part);
  xTaskNotifyGive(job->notify);
  vTaskDelete(nullptr);
}

void benchmark() {
  header("Benchmark de doble núcleo");
  Serial.printf("  Contando números primos menores que %lu...\n", (unsigned long)PRIME_LIMIT);

  int64_t t0 = esp_timer_get_time();
  uint32_t single = countPrimes(-1);
  int64_t tSingle = esp_timer_get_time() - t0;
  Serial.printf("  1 núcleo  : %lu primos en %lld ms\n", (unsigned long)single, tSingle / 1000);

  PrimeJob a = {0, 0, xTaskGetCurrentTaskHandle()};
  PrimeJob b = {1, 0, xTaskGetCurrentTaskHandle()};
  t0 = esp_timer_get_time();
  xTaskCreatePinnedToCore(primeTask, "primos0", 4096, &a, 1, nullptr, 0);
  xTaskCreatePinnedToCore(primeTask, "primos1", 4096, &b, 1, nullptr, 1);
  ulTaskNotifyTake(pdFALSE, portMAX_DELAY);
  ulTaskNotifyTake(pdFALSE, portMAX_DELAY);
  int64_t tDual = esp_timer_get_time() - t0;
  Serial.printf("  2 núcleos : %lu primos en %lld ms (núcleo 0: %lu, núcleo 1: %lu)\n",
                (unsigned long)(a.count + b.count), tDual / 1000, (unsigned long)a.count, (unsigned long)b.count);

  Serial.print("\n  1 núcleo  ");
  bar(tSingle, tSingle, 30, C_YELLOW);
  Serial.print("\n  2 núcleos ");
  bar(tDual, tSingle, 30, C_GREEN);
  Serial.printf("\n  Aceleración: " C_BOLD "%.2fx" C_RESET "\n", (double)tSingle / tDual);
}

// ---------- táctil ----------

void touchLive() {
  header("Sensores táctiles capacitivos");
  uint32_t base[touchCount];
  for (int i = 0; i < touchCount; i++) {
    uint32_t sum = 0;
    for (int k = 0; k < 8; k++) sum += touchRead(touchPins[i]);
    base[i] = max<uint32_t>(sum / 8, 1);
  }
  Serial.println("  Toca con el dedo la patita de cualquiera de estos pines.");
  Serial.println(C_DIM "  El valor baja cuando lo tocas. Pulsa cualquier tecla para salir." C_RESET "\n");
  for (int i = 0; i < touchCount; i++) Serial.println();

  flushInput();
  while (!Serial.available()) {
    Serial.printf("\033[%dA", touchCount);  // subir el cursor y redibujar
    for (int i = 0; i < touchCount; i++) {
      uint32_t v = touchRead(touchPins[i]);
      int drop = v >= base[i] ? 0 : (int)((base[i] - v) * 100 / base[i]);
      bool touched = drop > 25;
      Serial.printf("\r\033[K  GPIO%-2d %4lu ", touchPins[i], (unsigned long)v);
      bar(drop, 70, 30, touched ? C_GREEN : C_DIM);
      Serial.println(touched ? C_GREEN C_BOLD "  TOCADO" C_RESET : "");
    }
    delay(80);
  }
  flushInput();
}

// ---------- números aleatorios ----------

void randomDemo() {
  header("Números aleatorios por hardware");
  Serial.println(C_DIM "  esp_random() usa ruido físico del chip (más aleatorio con el radio encendido)" C_RESET);
  Serial.print("  8 valores de 32 bits: ");
  for (int i = 0; i < 8; i++) Serial.printf("%08lX ", (unsigned long)esp_random());

  const int ROLLS = 60000;
  int faces[6] = {0};
  for (int i = 0; i < ROLLS; i++) faces[esp_random() % 6]++;
  Serial.printf("\n\n  %d tiradas de un dado (lo ideal es %d por cara):\n", ROLLS, ROLLS / 6);
  for (int f = 0; f < 6; f++) {
    Serial.printf("   %d ", f + 1);
    bar(faces[f], ROLLS / 6 * 11 / 10, 40, C_MAGENTA);
    Serial.printf(" %d\n", faces[f]);
  }
}

// ---------- LED ----------

void setLedMode(LedMode mode) {
  if (ledMode == LED_BREATHE) ledcDetach(LED_PIN);
  ledMode = mode;
  if (ledMode == LED_BREATHE) {
    ledcAttach(LED_PIN, 5000, 8);
  } else {
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);
  }
}

void updateLed() {
  uint32_t t = millis();
  if (ledMode == LED_BLINK) {
    digitalWrite(LED_PIN, (t / 250) % 2);
  } else if (ledMode == LED_BREATHE) {
    float phase = (t % 3000) / 3000.0f * 2 * PI;
    ledcWrite(LED_PIN, (uint32_t)((1 - cosf(phase)) / 2 * 255));
  }
}

void handleLed() {
  setLedMode((LedMode)((ledMode + 1) % 3));
  Serial.printf(C_MAGENTA "  [web] LED cambiado a: %s" C_RESET "\n", ledModeNames[ledMode]);
  server.sendHeader("Location", "/panel");
  server.send(303);
}

// ---------- OTA ----------
//
// Hay dos ranuras de aplicación (ota_0 y ota_1). Una actualización se escribe en la ranura
// libre y arranca "pendiente de verificar": si el chip se reinicia antes de que se valide,
// el bootloader vuelve solo a la imagen anterior.

const uint32_t OTA_STABLE_MS = 30000;    // tiempo estable antes de validar la imagen
const uint32_t OTA_DEADLINE_MS = 90000;  // si sigue sin validarse, se hace rollback explícito

// Sin esto el core marca la imagen como válida nada más arrancar
extern "C" bool verifyRollbackLater() { return true; }

bool otaPending = false;     // la imagen en ejecución espera su validación
bool otaNeedsWifi = false;   // hay red guardada, así que la validación exige Wi-Fi
bool otaStarted = false;
bool otaBusy = false;        // recibiendo una actualización
LedMode otaLedBefore = LED_OFF;
String webUpdateError;
bool webUpdating = false;
bool webUpdateOk = false;

const char *otaStateStr(const esp_partition_t *p) {
  esp_ota_img_states_t s;
  if (esp_ota_get_state_partition(p, &s) != ESP_OK) return "sin estado (arranque normal)";
  switch (s) {
    case ESP_OTA_IMG_NEW:            return "nueva";
    case ESP_OTA_IMG_PENDING_VERIFY: return "pendiente de verificar";
    case ESP_OTA_IMG_VALID:          return "válida";
    case ESP_OTA_IMG_INVALID:        return "inválida (revertida)";
    case ESP_OTA_IMG_ABORTED:        return "abortada (revertida)";
    default:                         return "sin estado (arranque normal)";
  }
}

// La ranura puede volver a la que se revirtió: tiene que ser una imagen y no estar marcada como mala
bool otaSlotUsable(const esp_partition_t *p) {
  esp_app_desc_t d;
  if (!p || esp_ota_get_partition_description(p, &d) != ESP_OK) return false;
  esp_ota_img_states_t s;
  if (esp_ota_get_state_partition(p, &s) == ESP_OK && (s == ESP_OTA_IMG_INVALID || s == ESP_OTA_IMG_ABORTED)) return false;
  return true;
}

// Se llama una vez al arrancar: detecta si la imagen está pendiente y si hubo un rollback
void otaBoot() {
  esp_ota_img_states_t s;
  otaPending = esp_ota_get_state_partition(esp_ota_get_running_partition(), &s) == ESP_OK && s == ESP_OTA_IMG_PENDING_VERIFY;
  String ssid, pass;
  otaNeedsWifi = loadCredentials(ssid, pass);

  // Se avisa una sola vez por rollback: se recuerda la imagen mala en la NVS y se olvida
  // cuando esa ranura deja de estar marcada como mala (por ejemplo, al subir otra imagen)
  const esp_partition_t *bad = esp_ota_get_last_invalid_partition();
  esp_app_desc_t d;
  prefs.begin("ota", false);
  if (bad && esp_ota_get_partition_description(bad, &d) == ESP_OK) {
    uint32_t id = (d.app_elf_sha256[0] << 24) | (d.app_elf_sha256[1] << 16) | (d.app_elf_sha256[2] << 8) | d.app_elf_sha256[3];
    if (prefs.getUInt("rbsha", 0) != id) {
      prefs.putUInt("rbsha", id);
      Serial.printf(C_YELLOW C_BOLD "  [ota] ROLLBACK: la imagen de %s (SHA %02x%02x%02x%02x) falló antes de validarse." C_RESET "\n",
                    bad->label, d.app_elf_sha256[0], d.app_elf_sha256[1], d.app_elf_sha256[2], d.app_elf_sha256[3]);
      Serial.printf(C_YELLOW "  [ota] volví a la versión anterior (%s)." C_RESET "\n", esp_ota_get_running_partition()->label);
    }
  } else {
    if (prefs.isKey("rbsha")) prefs.remove("rbsha");
  }
  prefs.end();

  if (otaPending)
    Serial.printf(C_CYAN "  [ota] imagen pendiente de verificar: se valida tras %lu s estables%s, o vuelvo a la anterior en %lu s." C_RESET "\n",
                  (unsigned long)(OTA_STABLE_MS / 1000), otaNeedsWifi ? " con Wi-Fi conectado" : "",
                  (unsigned long)(OTA_DEADLINE_MS / 1000));
#ifdef OTA_SIMULATE_FAILURE
  Serial.println(C_RED C_BOLD "  [ota] IMAGEN DEFECTUOSA DE PRUEBA (OTA_SIMULATE_FAILURE)." C_RESET);
  if (otaPending) Serial.println(C_RED "  [ota] me voy a caer en ~5 s, antes de validarme, para probar el rollback." C_RESET);
  else Serial.println(C_YELLOW "  [ota] no estoy pendiente de verificar (¿se grabó por USB?): no me caigo." C_RESET);
#endif
}

// Se llama desde el bucle: valida la imagen cuando está sana o la revierte si no lo logra
void otaHealthCheck() {
#ifdef OTA_SIMULATE_FAILURE
  if (otaPending && millis() > 5000) {
    Serial.println(C_RED C_BOLD "  [ota] simulando una caída (abort)..." C_RESET);
    Serial.flush();
    abort();
  }
#endif
  static uint32_t lastCheck = 0;
  if (!otaPending || otaBusy || millis() - lastCheck < 1000) return;
  lastCheck = millis();
  uint32_t t = millis();
  bool wifiOk = !otaNeedsWifi || WiFi.status() == WL_CONNECTED;
  if (t >= OTA_STABLE_MS && wifiOk) {
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    otaPending = false;
    if (err == ESP_OK)
      Serial.println(C_GREEN C_BOLD "  [ota] imagen validada: el rollback automático queda cancelado." C_RESET);
    else
      Serial.printf(C_RED "  [ota] no pude validar la imagen: %s" C_RESET "\n", esp_err_to_name(err));
  } else if (t >= OTA_DEADLINE_MS) {
    Serial.printf(C_RED "  [ota] la imagen no se pudo validar en %lu s: %s. Vuelvo a la versión anterior." C_RESET "\n",
                  (unsigned long)(OTA_DEADLINE_MS / 1000), wifiOk ? "motivo desconocido" : "el Wi-Fi guardado no conectó");
    Serial.flush();
    esp_ota_mark_app_invalid_rollback_and_reboot();
    // Solo llega aquí si no hay una imagen anterior a la que volver
    Serial.println(C_YELLOW "  [ota] no hay una imagen anterior válida: dejo esta como válida." C_RESET);
    esp_ota_mark_app_valid_cancel_rollback();
    otaPending = false;
  } else if (t % 10000 < 1000) {
    Serial.printf(C_DIM "  [ota] verificando la imagen... (%lu s%s)" C_RESET "\n", (unsigned long)(t / 1000),
                  wifiOk ? "" : ", esperando Wi-Fi");
  }
}

// Deep sleep y ULP terminan en un reinicio, que revertiría una imagen sin validar
bool otaBlocksReset() {
  if (!otaPending) return false;
  Serial.printf(C_YELLOW "\n  La imagen aún está pendiente de verificar (faltan %lu s como máximo): un reinicio ahora la revertiría.\n"
                "  Espera a que se valide (mira el estado con o) y vuelve a intentarlo." C_RESET "\n",
                (unsigned long)(OTA_DEADLINE_MS > millis() ? (OTA_DEADLINE_MS - millis()) / 1000 : 0));
  return true;
}

void startOta() {
  if (otaStarted) return;
  ArduinoOTA.setHostname(HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.setMdnsEnabled(false);  // el sketch ya llamó a MDNS.begin(): evita iniciarlo dos veces
  ArduinoOTA.onStart([]() {
    otaBusy = true;
    otaLedBefore = ledMode;
    setLedMode(LED_OFF);
    Serial.println(C_CYAN C_BOLD "\n  [ota] recibiendo una actualización desde el PC..." C_RESET);
  });
  ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
    static int lastPct = -1;
    int pct = total ? done * 100 / total : 0;
    if (pct / 10 != lastPct / 10 || done == total) Serial.printf(C_DIM "  [ota] %d %%" C_RESET "\n", pct);
    lastPct = pct;
  });
  ArduinoOTA.onEnd([]() {
    Serial.println(C_GREEN C_BOLD "  [ota] actualización recibida, reiniciando..." C_RESET);
  });
  ArduinoOTA.onError([](ota_error_t e) {
    const char *why = e == OTA_AUTH_ERROR ? "contraseña incorrecta" : e == OTA_BEGIN_ERROR ? "no pude iniciar la escritura"
                    : e == OTA_CONNECT_ERROR ? "falló la conexión" : e == OTA_RECEIVE_ERROR ? "falló la recepción"
                    : e == OTA_END_ERROR ? "falló el cierre (¿imagen dañada?)" : "error desconocido";
    Serial.printf(C_RED "  [ota] actualización rechazada: %s" C_RESET "\n", why);
    otaBusy = false;
    setLedMode(otaLedBefore);
  });
  ArduinoOTA.begin();
  if (mdnsStarted) MDNS.enableArduino(3232, true);
  otaStarted = true;
  Serial.printf(C_DIM "  [ota] listo para actualizar desde el PC (%s.local, puerto 3232, con contraseña)" C_RESET "\n", HOSTNAME);
}

// Página y subida del firmware por el navegador (con usuario y contraseña)
bool webAuthOk() { return server.authenticate("admin", OTA_PASSWORD); }

void handleUpdatePage() {
  if (!webAuthOk()) {
    server.requestAuthentication();
    return;
  }
  String b = "<h1>Actualizar firmware</h1><p class='sub'>Versión actual: " FW_VERSION " (" FW_BUILD ")</p>"
             "<input type='file' id='f' accept='.bin'>"
             "<button class='btn' id='go' onclick='up()'>Subir y actualizar</button>"
             "<progress id='pr' value='0' max='100' style='width:100%;margin-top:14px'></progress>"
             "<p id='st' class='small'>Elige el archivo firmware.bin que genera PlatformIO.</p>"
             "<p class='small'>Si la nueva versión no logra validarse, el ESP32 vuelve solo a la anterior.</p>"
             "<a class='link' href='/panel'>← Volver al panel</a>"
             "<script>function up(){const f=document.getElementById('f').files[0];"
             "if(!f){st.textContent='Elige primero un archivo .bin';return}"
             "const x=new XMLHttpRequest();x.open('POST','/update');"
             "x.upload.onprogress=e=>{if(e.lengthComputable){pr.value=e.loaded*100/e.total;st.textContent=Math.round(pr.value)+' %'}};"
             "x.onload=()=>{st.innerHTML=(x.status==200?\"<span class='ok'>✔ \":\"<span class='err'>✖ \")+x.responseText+'</span>';"
             "go.disabled=x.status==200};"
             "x.onerror=()=>{st.innerHTML=\"<span class='err'>✖ Se perdió la conexión con el ESP32.</span>\";go.disabled=false};"
             "const d=new FormData();d.append('firmware',f,f.name);go.disabled=true;x.send(d)}</script>";
  sendPage(b);
}

void handleUpdateUpload() {
  HTTPUpload &u = server.upload();
  if (u.status == UPLOAD_FILE_START) {
    webUpdateError = "";
    webUpdateOk = false;
    if (!webAuthOk()) {  // este manejador corre antes que handleUpdateDone: sin permiso no se escribe nada
      webUpdateError = "no autorizado";
      return;
    }
    Serial.printf(C_CYAN C_BOLD "\n  [ota] %s subió %s por la web..." C_RESET "\n", server.client().remoteIP().toString().c_str(), u.filename.c_str());
    otaBusy = true;
    otaLedBefore = ledMode;
    setLedMode(LED_OFF);
    if (Update.begin(UPDATE_SIZE_UNKNOWN)) webUpdating = true;
    else webUpdateError = Update.errorString();
  } else if (u.status == UPLOAD_FILE_WRITE) {
    if (webUpdating && Update.write(u.buf, u.currentSize) != u.currentSize) webUpdateError = Update.errorString();
  } else if (u.status == UPLOAD_FILE_END) {
    if (webUpdating && webUpdateError.isEmpty()) {
      if (Update.end(true)) webUpdateOk = true;
      else webUpdateError = Update.errorString();
    }
  } else if (u.status == UPLOAD_FILE_ABORTED) {
    webUpdateError = "subida interrumpida";
  }
  // Al terminar o fallar se cierra la escritura; si no fue un éxito, se devuelve el LED y la verificación
  bool finished = u.status == UPLOAD_FILE_END || u.status == UPLOAD_FILE_ABORTED || !webUpdateError.isEmpty();
  if (finished && webUpdating) {
    if (!webUpdateOk) Update.abort();
    webUpdating = false;
  }
  if (finished && !webUpdateOk && otaBusy) {
    otaBusy = false;
    setLedMode(otaLedBefore);
  }
}

void handleUpdateDone() {
  if (!webAuthOk()) {
    server.requestAuthentication();
    return;
  }
  if (!webUpdateOk) {
    String why = webUpdateError.isEmpty() ? String("no llegó ningún archivo") : webUpdateError;
    Serial.printf(C_RED "  [ota] actualización web rechazada: %s" C_RESET "\n", why.c_str());
    server.send(500, "text/plain; charset=utf-8", "Falló la actualización: " + why);
    return;
  }
  Serial.println(C_GREEN C_BOLD "  [ota] actualización web recibida, reiniciando..." C_RESET);
  server.send(200, "text/plain; charset=utf-8", "Listo. El ESP32 se reinicia; en unos segundos vuelve a estar disponible.");
  delay(500);
  ESP.restart();
}

void otaMenu() {
  header("Actualizaciones OTA");
  const esp_partition_t *run = esp_ota_get_running_partition();
  const esp_partition_t *other = esp_ota_get_next_update_partition(nullptr);
  const esp_app_desc_t *me = esp_app_get_description();
  Serial.printf("  Ejecutando   : %s en 0x%lX (%lu KB)\n", run->label, (unsigned long)run->address, (unsigned long)(run->size / 1024));
  Serial.printf("  Versión      : " C_BOLD "%s" C_RESET ", compilada %s, SHA %02x%02x%02x%02x\n", FW_VERSION, FW_BUILD,
                me->app_elf_sha256[0], me->app_elf_sha256[1], me->app_elf_sha256[2], me->app_elf_sha256[3]);
  if (otaPending) {
    Serial.printf("  Estado       : " C_YELLOW "pendiente de verificar" C_RESET " (límite en %lu s, necesita %lu s estables%s)\n",
                  (unsigned long)(OTA_DEADLINE_MS > millis() ? (OTA_DEADLINE_MS - millis()) / 1000 : 0),
                  (unsigned long)(OTA_STABLE_MS / 1000), otaNeedsWifi ? " y Wi-Fi conectado" : "");
  } else {
    Serial.printf("  Estado       : %s\n", otaStateStr(run));
  }
  if (other) {
    esp_app_desc_t d;
    Serial.printf("  Otra ranura  : %s en 0x%lX (%lu KB), %s\n", other->label, (unsigned long)other->address,
                  (unsigned long)(other->size / 1024), otaStateStr(other));
    if (esp_ota_get_partition_description(other, &d) == ESP_OK)
      Serial.printf("  Su versión   : SHA %02x%02x%02x%02x\n", d.app_elf_sha256[0], d.app_elf_sha256[1], d.app_elf_sha256[2], d.app_elf_sha256[3]);
    else
      Serial.println("  Su versión   : vacía (no hay ninguna imagen)");
  }
  const esp_partition_t *bad = esp_ota_get_last_invalid_partition();
  if (bad) Serial.printf("  Último rollback: la imagen de %s se revirtió por fallar antes de validarse\n", bad->label);
  else Serial.println("  Último rollback: ninguno");
  Serial.println("\n  Para actualizar:");
  Serial.printf ("   - desde el PC: " C_BOLD "pio run -e esp32dev-ota -t upload" C_RESET " (o --upload-port <ip>)\n");
  Serial.printf ("   - desde el navegador: " C_BOLD "http://%s/update" C_RESET " (usuario admin)\n",
                 WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "<ip>");

  if (!otaSlotUsable(other)) {
    Serial.println(C_DIM "\n  No hay una versión anterior válida a la que volver." C_RESET);
    return;
  }
  Serial.println(C_BOLD "\n  Pulsa " C_YELLOW "v" C_RESET C_BOLD " para volver a la versión anterior, o cualquier otra tecla para salir." C_RESET);
  flushInput();
  while (!Serial.available()) {  // mientras espera, el resto sigue funcionando
    if (serverStarted) server.handleClient();
    if (otaStarted) ArduinoOTA.handle();
    otaHealthCheck();
    delay(5);
  }
  if (tolower(Serial.read()) != 'v') {
    flushInput();
    return;
  }
  flushInput();
  Serial.println(C_YELLOW "  Arrancando la versión anterior..." C_RESET);
  Serial.flush();
  if (otaPending) {
    esp_ota_mark_app_invalid_rollback_and_reboot();
  } else {
    esp_err_t err = esp_ota_set_boot_partition(other);
    if (err == ESP_OK) {
      delay(200);
      ESP.restart();
    }
    Serial.printf(C_RED "  No pude cambiar de ranura: %s" C_RESET "\n", esp_err_to_name(err));
    return;
  }
  Serial.println(C_RED "  No pude volver atrás." C_RESET);
}

// ---------- deep sleep ----------

void deepSleep() {
  if (otaBlocksReset()) return;
  header("Deep sleep");
  Serial.println("  Me duermo 10 s consumiendo unos microamperios. Al despertar es como un reinicio,");
  Serial.println("  pero el contador de arranques (en RAM del RTC) se conserva.");
  Serial.flush();
  esp_sleep_enable_timer_wakeup(10ULL * 1000000);
  esp_deep_sleep_start();
}

// ---------- coprocesador ULP ----------
//
// El ULP FSM es un procesador mínimo que vive en el dominio RTC y sigue corriendo en
// deep sleep. Se programa con macros (casi ensamblador) y comparte con el CPU los
// 512 bytes reservados de RAM RTC lenta: primero van los datos y luego el programa.

const int ULP_TICK_MS = 250;     // periodo pedido; el real se mide al despertar
const int ULP_SAMPLE_EVERY = 1;  // lee el sensor táctil en cada despertar
const int ULP_SAMPLES_N = 60;    // con 60 lecturas despierta al CPU
const int ULP_TOUCH_PIN = 4;     // GPIO4 = T0
const int LED_RTC_GPIO = 12;     // GPIO2 (LED) es RTC_GPIO12 en el ESP32

enum { ULP_TICKS = 0, ULP_IDX = 1, ULP_SUB = 2, ULP_SAMPLES = 4 };  // offsets en palabras de 32 bits
const int ULP_PROG_START = ULP_SAMPLES + ULP_SAMPLES_N;

RTC_DATA_ATTR int64_t ulpSleepStartUs = 0;

// El reloj del sistema sigue contando durante el deep sleep (lo lleva el RTC)
int64_t wallClockUs() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

void ulpDemo() {
  if (otaBlocksReset()) return;
  header("Coprocesador ULP");
  Serial.println("  El CPU principal se va a deep sleep. El ULP queda solo:");
  Serial.println("   - hace parpadear el LED azul");
  Serial.printf ("   - lee el sensor táctil de GPIO%d y guarda cada lectura en la RAM RTC\n", ULP_TOUCH_PIN);
  Serial.printf ("   - con %d lecturas despierta al CPU, que te dibuja la gráfica\n", ULP_SAMPLES_N);
  Serial.printf (C_BOLD "  Mientras duerme, toca la patita GPIO%d unas cuantas veces." C_RESET "\n", ULP_TOUCH_PIN);

  enum { L_LED_OFF = 1, L_LED_DONE, L_HALT, L_WAIT_RDY, L_WAIT_TOUCH };
  const ulp_insn_t program[] = {
      I_MOVI(R3, 0),  // R3 = base de los datos
      // ticks++ y el LED cambia de estado en cada despertar
      I_LD(R0, R3, ULP_TICKS),
      I_ADDI(R0, R0, 1),
      I_ST(R0, R3, ULP_TICKS),
      I_ANDI(R0, R0, 1),
      M_BGE(L_LED_OFF, 1),
      I_WR_REG_BIT(RTC_GPIO_OUT_W1TS_REG, RTC_GPIO_OUT_DATA_W1TS_S + LED_RTC_GPIO, 1),
      M_BX(L_LED_DONE),
      M_LABEL(L_LED_OFF),
      I_WR_REG_BIT(RTC_GPIO_OUT_W1TC_REG, RTC_GPIO_OUT_DATA_W1TC_S + LED_RTC_GPIO, 1),
      M_LABEL(L_LED_DONE),
      // cada ULP_SAMPLE_EVERY despertares, medir
      I_LD(R0, R3, ULP_SUB),
      I_ADDI(R0, R0, 1),
      I_ST(R0, R3, ULP_SUB),
      M_BL(L_HALT, ULP_SAMPLE_EVERY),
      I_MOVI(R0, 0),
      I_ST(R0, R3, ULP_SUB),
      // el sensor táctil mide solo (lo controla el hardware); el ULP espera a que termine
      // una medición y lee el valor de T0
      M_LABEL(L_WAIT_TOUCH),
      I_RD_REG(SENS_SAR_TOUCH_CTRL2_REG, SENS_TOUCH_MEAS_DONE_S, SENS_TOUCH_MEAS_DONE_S),
      M_BL(L_WAIT_TOUCH, 1),
      I_RD_REG(SENS_SAR_TOUCH_OUT1_REG, SENS_TOUCH_MEAS_OUT0_S, SENS_TOUCH_MEAS_OUT0_S + 15),
      I_MOVR(R1, R0),
      I_LD(R2, R3, ULP_IDX),
      I_ST(R1, R2, ULP_SAMPLES),  // samples[idx] = lectura táctil
      I_ADDI(R2, R2, 1),
      I_ST(R2, R3, ULP_IDX),
      I_MOVR(R0, R2),
      M_BL(L_HALT, ULP_SAMPLES_N),
      // buffer lleno: esperar a que el chip acepte el despertar, despertar al CPU y detenerse
      M_LABEL(L_WAIT_RDY),
      I_RD_REG(RTC_CNTL_LOW_POWER_ST_REG, RTC_CNTL_RDY_FOR_WAKEUP_S, RTC_CNTL_RDY_FOR_WAKEUP_S),
      M_BL(L_WAIT_RDY, 1),
      I_WAKE(),
      I_END(),
      M_LABEL(L_HALT),
      I_HALT(),
  };

  for (int i = 0; i < ULP_PROG_START; i++) RTC_SLOW_MEM[i] = 0;

  // El LED pasa al dominio RTC para que el ULP lo controle en deep sleep
  setLedMode(LED_OFF);
  rtc_gpio_init(GPIO_NUM_2);
  rtc_gpio_set_direction(GPIO_NUM_2, RTC_GPIO_MODE_OUTPUT_ONLY);
  rtc_gpio_set_level(GPIO_NUM_2, 0);

  // touchRead() deja el sensor táctil midiendo de forma continua por hardware
  Serial.printf(C_DIM "  Valor táctil de GPIO%d sin tocar: %lu" C_RESET "\n", ULP_TOUCH_PIN,
                (unsigned long)touchRead(ULP_TOUCH_PIN));

  size_t size = sizeof(program) / sizeof(ulp_insn_t);
  esp_err_t err = ulp_process_macros_and_load(ULP_PROG_START, program, &size);
  if (err != ESP_OK) {
    Serial.printf(C_RED "  No pude cargar el programa del ULP: %s" C_RESET "\n", esp_err_to_name(err));
    rtc_gpio_deinit(GPIO_NUM_2);
    setLedMode(LED_OFF);
    return;
  }
  Serial.printf(C_DIM "  Programa del ULP: %d instrucciones en la palabra %d de la RAM RTC" C_RESET "\n", (int)size, ULP_PROG_START);

  ulp_set_wakeup_period(0, ULP_TICK_MS * 1000);
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
  // El sensor táctil cuenta con el reloj de 8 MHz: sin él, las mediciones salen cortadas
  esp_sleep_pd_config(ESP_PD_DOMAIN_RC_FAST, ESP_PD_OPTION_ON);
  esp_sleep_enable_ulp_wakeup();
  // Respaldo: si el despertar del ULP se perdiera, el temporizador despierta al CPU igual
  esp_sleep_enable_timer_wakeup(120ULL * 1000000);
  ulpPending = true;
  ulpSleepStartUs = wallClockUs();
  ulp_run(ULP_PROG_START);
  Serial.println("  Durmiendo...");
  Serial.flush();
  esp_deep_sleep_start();
}

void showUlpResults(esp_sleep_wakeup_cause_t cause) {
  ulp_timer_stop();
  ulpPending = false;
  uint32_t ticks = RTC_SLOW_MEM[ULP_TICKS] & 0xFFFF;
  int n = min<int>(RTC_SLOW_MEM[ULP_IDX] & 0xFFFF, ULP_SAMPLES_N);

  header("Coprocesador ULP: resultados");
  Serial.printf("  Me despertó: %s\n", cause == ESP_SLEEP_WAKEUP_ULP ? "el ULP (buffer lleno)" : "el temporizador de respaldo");
  float slept = (wallClockUs() - ulpSleepStartUs) / 1e6f;
  Serial.printf("  El CPU durmió %.1f s. Mientras, el ULP corrió %lu veces (una cada %lu ms) y guardó %d lecturas.\n",
                slept, (unsigned long)ticks, ticks ? (unsigned long)(slept * 1000 / ticks) : 0UL, n);
  if (n == 0) return;

  uint16_t vals[ULP_SAMPLES_N];
  uint16_t lo = 0xFFFF, hi = 0;
  uint32_t sum = 0;
  for (int i = 0; i < n; i++) {
    vals[i] = RTC_SLOW_MEM[ULP_SAMPLES + i] & 0xFFFF;
    lo = min(lo, vals[i]);
    hi = max(hi, vals[i]);
    sum += vals[i];
  }
  Serial.printf("  Sensor táctil GPIO%d (baja cuando lo tocas): mín %u, máx %u, promedio %.1f\n\n", ULP_TOUCH_PIN,
                lo, hi, (double)sum / n);

  const int ROWS = 8;
  for (int r = ROWS; r >= 1; r--) {
    if (r == ROWS) Serial.printf("  %5u ┤", hi);
    else if (r == 1) Serial.printf("  %5u ┤", lo);
    else Serial.print("        │");
    Serial.print(C_CYAN);
    for (int i = 0; i < n; i++) {
      int h = hi == lo ? ROWS / 2 : 1 + (vals[i] - lo) * (ROWS - 1) / (hi - lo);
      Serial.print(h >= r ? "█" : " ");
    }
    Serial.println(C_RESET);
  }
  Serial.print("        └");
  for (int i = 0; i < n; i++) Serial.print("─");
  Serial.printf("\n         0 s%*d s\n", n - 4, (int)(slept + 0.5f));
}

// ---------- arranque y bucle ----------

void setup() {
  Serial.begin(115200);
  delay(300);
  bootCount++;
  rtc_gpio_deinit(GPIO_NUM_2);  // por si el ULP lo dejó en el dominio RTC
  setLedMode(LED_OFF);

  server.on("/", handleRoot);
  server.on("/panel", handlePanel);
  server.on("/led", handleLed);
  server.on("/scan", handleScan);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/status", handleStatus);
  server.on("/update", HTTP_GET, handleUpdatePage);
  server.on("/update", HTTP_POST, handleUpdateDone, handleUpdateUpload);
  server.onNotFound(handleNotFound);

  WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t info) {
    const uint8_t *m = info.wifi_ap_staconnected.mac;
    Serial.printf(C_GREEN "  [portal] se conectó %02X:%02X:%02X:%02X:%02X:%02X" C_RESET "\n", m[0], m[1], m[2], m[3], m[4], m[5]);
  }, ARDUINO_EVENT_WIFI_AP_STACONNECTED);
  WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t info) {
    const uint8_t *m = info.wifi_ap_stadisconnected.mac;
    Serial.printf(C_YELLOW "  [portal] se desconectó %02X:%02X:%02X:%02X:%02X:%02X" C_RESET "\n", m[0], m[1], m[2], m[3], m[4], m[5]);
  }, ARDUINO_EVENT_WIFI_AP_STADISCONNECTED);
  WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t info) {
    uint8_t r = info.wifi_sta_disconnected.reason;
    if (r != WIFI_REASON_ASSOC_LEAVE) lastDisconnectReason = r;  // ASSOC_LEAVE = desconexión pedida por nosotros
  }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);

  Serial.println("\n\n" C_BOLD C_GREEN "ESP32 listo." C_RESET);
  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  if (ulpPending && (cause == ESP_SLEEP_WAKEUP_ULP || cause == ESP_SLEEP_WAKEUP_TIMER))
    showUlpResults(cause);
  else if (cause == ESP_SLEEP_WAKEUP_TIMER)
    Serial.printf(C_GREEN "Desperté del deep sleep. Arranque número %lu." C_RESET "\n", (unsigned long)bootCount);
  ulpPending = false;
  otaBoot();

  WiFi.setHostname(HOSTNAME);
  String ssid, pass;
  if (loadCredentials(ssid, pass)) {
    startConnect(ssid, pass, false);  // si falla, updateConnection() abre el portal
  } else {
    Serial.println("  No hay red Wi-Fi guardada todavía.");
    startPortal();
  }
  printMenu();
}

void loop() {
  if (portalActive) dns.processNextRequest();
  if (serverStarted) server.handleClient();
  if (otaStarted) ArduinoOTA.handle();
  updateConnection();
  otaHealthCheck();
  if (portalCloseAt && (int32_t)(millis() - portalCloseAt) >= 0) stopPortal();
  updateLed();

  if (!Serial.available()) {
    delay(2);
    return;
  }
  char c = tolower(Serial.read());
  switch (c) {
    case 'i': printInfo(); break;
    case 'n': printNetwork(); break;
    case 'a':
      if (portalActive) stopPortal();
      else startPortal();
      break;
    case 'x':
      forgetCredentials();
      WiFi.disconnect();
      connState = CONN_IDLE;
      linkUp = false;
      Serial.println("\n  Red guardada borrada y Wi-Fi desconectado. Pulsa a para abrir el portal.");
      break;
    case 'w': scanWifi(); break;
    case 'b': scanBle(); break;
    case 'c': benchmark(); break;
    case 't': touchLive(); break;
    case 'r': randomDemo(); break;
    case 'l':
      setLedMode((LedMode)((ledMode + 1) % 3));
      Serial.printf("\n  LED: %s\n", ledModeNames[ledMode]);
      break;
    case 's': deepSleep(); break;
    case 'u': ulpDemo(); break;
    case 'o': otaMenu(); break;
    case 'h': case '?': printMenu(); break;
    case '\r': case '\n': case ' ': return;
    default: Serial.printf("\n  Tecla '%c' no reconocida. Pulsa h para ver el menú.\n", c); return;
  }
  Serial.println(C_DIM "\n  (h = menú)" C_RESET);
}
