#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <WiFiUdp.h>
#include <WiFiClient.h>
#include <WiFiServer.h>
#include <PubSubClient.h>

// Fallback-Defaults falls nicht in platformio.ini definiert
#ifndef DEFAULT_INPUT_PINS
#define DEFAULT_INPUT_PINS "16,17,18,19"
#endif
#ifndef DEFAULT_OUTPUT_PINS
#define DEFAULT_OUTPUT_PINS "25,26,27,33"
#endif

/*
  Serial Logging
  --------------
  LOG_LEVEL:
    0 = ERROR
    1 = WARN
    2 = INFO
    3 = DEBUG
*/
#ifndef LOG_LEVEL
#define LOG_LEVEL 2
#endif

// Ring-Buffer für Web-Logs
#define LOG_BUFFER_SIZE 50
static String logBuffer[LOG_BUFFER_SIZE];
static int logBufferIndex = 0;
static int logBufferCount = 0;

static inline const char* lvlChar(int l) {
  switch (l) { case 0: return "E"; case 1: return "W"; case 2: return "I"; default: return "D"; }
}
static inline void logf(int level, const char* tag, const char* fmt, ...) {
#if LOG_LEVEL >= 0
  if (level > LOG_LEVEL) return;
  uint32_t ms = millis();

  va_list ap;
  va_start(ap, fmt);
  char buffer[256];
  vsnprintf(buffer, sizeof(buffer), fmt, ap);
  va_end(ap);

  // Serial ausgeben
  Serial.printf("[%10lu][%s][%s] ", (unsigned long)ms, lvlChar(level), tag);
  Serial.print(buffer);
  Serial.println();

  // In Ring-Buffer speichern
  String logLine = "[" + String((unsigned long)ms) + "][" + String(lvlChar(level)) + "][" + String(tag) + "] " + String(buffer);
  logBuffer[logBufferIndex] = logLine;
  logBufferIndex = (logBufferIndex + 1) % LOG_BUFFER_SIZE;
  if (logBufferCount < LOG_BUFFER_SIZE) logBufferCount++;
#else
  (void)level; (void)tag; (void)fmt;
#endif
}

#define LOGE(tag, fmt, ...) logf(0, tag, fmt, ##__VA_ARGS__)
#define LOGW(tag, fmt, ...) logf(1, tag, fmt, ##__VA_ARGS__)
#define LOGI(tag, fmt, ...) logf(2, tag, fmt, ##__VA_ARGS__)
#define LOGD(tag, fmt, ...) logf(3, tag, fmt, ##__VA_ARGS__)

static Preferences prefs;
static WebServer server(80);

// --- Transports ---
static WiFiUDP udp;

// LAN Library: Rocrail connects to ESP32 TCP server
static WiFiServer lanServer(5550);
static WiFiClient lanClient;
static String lanRxBuf;

// MQTT
static WiFiClient mqttNet;
static PubSubClient mqtt(mqttNet);

// ---------- Transport selection ----------
enum IoMode : uint8_t { MODE_UUDP = 0, MODE_LAN = 1, MODE_MQTT = 2 };

static const char* modeName(IoMode m) {
  switch (m) {
    case MODE_UUDP: return "UUDP";
    case MODE_LAN:  return "LAN";
    case MODE_MQTT: return "MQTT";
    default:        return "UNKNOWN";
  }
}

struct Config {
  // WiFi STA
  String staSsid;
  String staPass;

  // Common ID
  String iid; // e.g. "esp32"

  // Inputs -> <fb addr=...>
  uint16_t inAddrBase; // addr = base + index
  String inPinsCsv;
  String inInvCsv;
  uint16_t debounceMs;

  // Outputs -> <co addr=... port=... cmd=on/off>
  uint16_t outAddr;
  String outPinsCsv;
  String outInvCsv;

  // Mode selection
  IoMode ioMode;

  // --- UUDP settings ---
  String rocHost;     // Rocrail host/ip
  uint16_t rocPort;   // usually 21111
  uint16_t udpLocalPort;
  bool sendTwice;
  uint16_t logonEverySec;

  // --- LAN Library settings (ESP32 TCP server) ---
  uint16_t lanPort;         // TCP port for Rocrail to connect to
  uint16_t lanKeepaliveSec; // optional heartbeat (0=off)

  // --- MQTT settings ---
  String mqttHost;
  uint16_t mqttPort;
  String mqttUser;
  String mqttPass;
  String mqttTopicBase;   // default "rocrail/service"
  uint8_t mqttQoS;        // 0 or 1 (we do QoS=0 in PubSubClient, but keep setting for future)
  uint16_t mqttKeepalive; // seconds
};

static Config cfg;

struct PinState {
  int pin = -1;
  bool invert = false;

  bool lastRaw = false;
  bool stable = false;
  uint32_t lastChangeMs = 0;
};

struct OutState {
  int pin = -1;
  bool invert = false;
  bool logicalOn = false;
};

static PinState inStates[16];
static size_t inCount = 0;

static OutState outStates[16];
static size_t outCount = 0;

static uint32_t lastLogonMs = 0;
static uint32_t lastStatusMs = 0;
static uint32_t lastLanKeepaliveMs = 0;
static uint32_t lastMqttReconnectMs = 0;

// Counters
static uint32_t cntTx = 0;
static uint32_t cntRx = 0;
static uint32_t cntFbEvents = 0;
static uint32_t cntCoCmds = 0;
static uint32_t cntParseDrop = 0;
static uint32_t cntIidDrop = 0;
static uint32_t cntLanConn = 0;
static uint32_t cntLanDisc = 0;
static uint32_t cntMqttConn = 0;
static uint32_t cntMqttDisc = 0;

// ----------------------------- Helpers -----------------------------

static String htmlEscape(const String& s) {
  String out; out.reserve(s.length());
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    switch (c) {
      case '&': out += F("&amp;"); break;
      case '<': out += F("&lt;"); break;
      case '>': out += F("&gt;"); break;
      case '"': out += F("&quot;"); break;
      default: out += c; break;
    }
  }
  return out;
}

static int splitCsvInt(const String& csv, int* out, int maxN) {
  int n = 0;
  int start = 0;
  while (start < (int)csv.length() && n < maxN) {
    int comma = csv.indexOf(',', start);
    String token = (comma == -1) ? csv.substring(start) : csv.substring(start, comma);
    token.trim();
    if (token.length() > 0) out[n++] = token.toInt();
    if (comma == -1) break;
    start = comma + 1;
  }
  return n;
}

static int splitCsvBool(const String& csv, bool* out, int maxN) {
  int n = 0;
  int start = 0;
  while (start < (int)csv.length() && n < maxN) {
    int comma = csv.indexOf(',', start);
    String token = (comma == -1) ? csv.substring(start) : csv.substring(start, comma);
    token.trim();
    if (token.length() > 0) out[n++] = (token.toInt() != 0);
    if (comma == -1) break;
    start = comma + 1;
  }
  return n;
}

static String getAttr(const String& xml, const char* key) {
  String k = String(key) + "=\"";
  int p = xml.indexOf(k);
  if (p < 0) return "";
  p += k.length();
  int e = xml.indexOf('"', p);
  if (e < 0) return "";
  return xml.substring(p, e);
}

static bool parseOnOff(const String& cmdS, bool& onOut) {
  if (cmdS == "on" || cmdS == "1" || cmdS == "true") { onOut = true; return true; }
  if (cmdS == "off" || cmdS == "0" || cmdS == "false") { onOut = false; return true; }
  return false;
}

// ----------------------------- Config -----------------------------

static void loadConfig() {
  prefs.begin("rocrail", true);

  cfg.staSsid = prefs.getString("ssid", "");
  cfg.staPass = prefs.getString("pass", "");

  cfg.iid = prefs.getString("iid", "esp32");

  cfg.inAddrBase  = (uint16_t)prefs.getUShort("ibase", 100);
  cfg.inPinsCsv   = prefs.getString("ipins", DEFAULT_INPUT_PINS);
  cfg.inInvCsv    = prefs.getString("iinv",  "0,0,0,0");
  cfg.debounceMs  = (uint16_t)prefs.getUShort("deb", 40);

  cfg.outAddr     = (uint16_t)prefs.getUShort("oaddr", 1);
  cfg.outPinsCsv  = prefs.getString("opins", DEFAULT_OUTPUT_PINS);
  cfg.outInvCsv   = prefs.getString("oinv",  "0,0,0,0");

  cfg.ioMode = (IoMode)prefs.getUChar("mode", (uint8_t)MODE_UUDP);

  // UUDP
  cfg.rocHost = prefs.getString("host", "192.168.1.10");
  cfg.rocPort = (uint16_t)prefs.getUShort("rport", 21111);
  cfg.udpLocalPort  = (uint16_t)prefs.getUShort("lport", 21112);
  cfg.sendTwice     = prefs.getBool("twice", true);
  cfg.logonEverySec = (uint16_t)prefs.getUShort("logsec", 60);

  // LAN
  cfg.lanPort = (uint16_t)prefs.getUShort("lanport", 5550);
  cfg.lanKeepaliveSec = (uint16_t)prefs.getUShort("lankeep", 0);

  // MQTT
  cfg.mqttHost = prefs.getString("mqhost", "192.168.1.10");
  cfg.mqttPort = (uint16_t)prefs.getUShort("mqport", 1883);
  cfg.mqttUser = prefs.getString("mquser", "");
  cfg.mqttPass = prefs.getString("mqpass", "");
  cfg.mqttTopicBase = prefs.getString("mqbase", "rocrail/service");
  cfg.mqttQoS = (uint8_t)prefs.getUChar("mqqos", 0);
  cfg.mqttKeepalive = (uint16_t)prefs.getUShort("mqkeep", 30);

  prefs.end();
}

static void saveConfigFromHttp() {
  prefs.begin("rocrail", false);

  prefs.putString("ssid", server.arg("ssid"));
  prefs.putString("pass", server.arg("pass"));

  prefs.putString("iid", server.arg("iid"));

  prefs.putUShort("ibase", (uint16_t)server.arg("ibase").toInt());
  prefs.putString("ipins", server.arg("ipins"));
  prefs.putString("iinv",  server.arg("iinv"));
  prefs.putUShort("deb", (uint16_t)server.arg("deb").toInt());

  prefs.putUShort("oaddr", (uint16_t)server.arg("oaddr").toInt());
  prefs.putString("opins", server.arg("opins"));
  prefs.putString("oinv",  server.arg("oinv"));

  prefs.putUChar("mode", (uint8_t)server.arg("mode").toInt());

  // UUDP
  prefs.putString("host", server.arg("host"));
  prefs.putUShort("rport", (uint16_t)server.arg("rport").toInt());
  prefs.putUShort("lport", (uint16_t)server.arg("lport").toInt());
  prefs.putBool("twice", server.arg("twice") == "1");
  prefs.putUShort("logsec", (uint16_t)server.arg("logsec").toInt());

  // LAN
  prefs.putUShort("lanport", (uint16_t)server.arg("lanport").toInt());
  prefs.putUShort("lankeep", (uint16_t)server.arg("lankeep").toInt());

  // MQTT
  prefs.putString("mqhost", server.arg("mqhost"));
  prefs.putUShort("mqport", (uint16_t)server.arg("mqport").toInt());
  prefs.putString("mquser", server.arg("mquser"));
  prefs.putString("mqpass", server.arg("mqpass"));
  prefs.putString("mqbase", server.arg("mqbase"));
  prefs.putUChar("mqqos", (uint8_t)server.arg("mqqos").toInt());
  prefs.putUShort("mqkeep", (uint16_t)server.arg("mqkeep").toInt());

  prefs.end();
}

// ----------------------------- Web UI -----------------------------

static void handleRoot() {
  String page; page.reserve(5200);
  page += F("<!doctype html><html><head><meta charset='utf-8'>"
            "<meta name='viewport' content='width=device-width,initial-scale=1'>"
            "<title>ESP32 Rocrail IO</title></head><body>");
  page += F("<h2>ESP32 Rocrail IO</h2>");

  page += F("<form method='POST' action='/save'>");

  page += F("<fieldset><legend>WLAN (STA)</legend>");
  page += F("SSID:<br><input name='ssid' value='"); page += htmlEscape(cfg.staSsid); page += F("'><br>");
  page += F("Passwort:<br><input name='pass' type='password' value='"); page += htmlEscape(cfg.staPass); page += F("'><br>");
  page += F("</fieldset><br>");

  page += F("<fieldset><legend>Allgemein</legend>");
  page += F("IID (z.B. esp32):<br><input name='iid' value='"); page += htmlEscape(cfg.iid); page += F("'><br>");
  page += F("IO Modus:<br>");
  page += F("<select name='mode'>");
  page += String("<option value='0'") + (cfg.ioMode==MODE_UUDP?" selected":"") + ">UUDP (UDP)</option>";
  page += String("<option value='1'") + (cfg.ioMode==MODE_LAN ?" selected":"") + ">LAN Library (TCP)</option>";
  page += String("<option value='2'") + (cfg.ioMode==MODE_MQTT?" selected":"") + ">MQTT</option>";
  page += F("</select><br>");
  page += F("</fieldset><br>");

  page += F("<fieldset><legend>Inputs (IR DO)</legend>");
  page += F("Addr-Base (addr = base + index):<br><input name='ibase' value='"); page += String(cfg.inAddrBase); page += F("'><br>");
  page += F("Input GPIOs CSV:<br><input name='ipins' value='"); page += htmlEscape(cfg.inPinsCsv); page += F("'><br>");
  page += F("Invert CSV (0/1 passend zu Inputs):<br><input name='iinv' value='"); page += htmlEscape(cfg.inInvCsv); page += F("'><br>");
  page += F("Debounce ms:<br><input name='deb' value='"); page += String(cfg.debounceMs); page += F("'><br>");
  page += F("</fieldset><br>");

  page += F("<fieldset><legend>Outputs (&lt;co&gt;)</legend>");
  page += F("Output addr (match &lt;co addr='...'>):<br><input name='oaddr' value='"); page += String(cfg.outAddr); page += F("'><br>");
  page += F("Output GPIOs CSV (Index = port):<br><input name='opins' value='"); page += htmlEscape(cfg.outPinsCsv); page += F("'><br>");
  page += F("Invert CSV (0/1 passend zu Outputs):<br><input name='oinv' value='"); page += htmlEscape(cfg.outInvCsv); page += F("'><br>");
  page += F("</fieldset><br>");

  page += F("<fieldset><legend>UUDP (UDP)</legend>");
  page += F("Rocrail Host/IP:<br><input name='host' value='"); page += htmlEscape(cfg.rocHost); page += F("'><br>");
  page += F("Rocrail UDP Port:<br><input name='rport' value='"); page += String(cfg.rocPort); page += F("'><br>");
  page += F("Local UDP Port (listen):<br><input name='lport' value='"); page += String(cfg.udpLocalPort); page += F("'><br>");
  page += F("Logon alle X Sekunden (0 = nur beim Start):<br><input name='logsec' value='"); page += String(cfg.logonEverySec); page += F("'><br>");
  page += F("UDP doppelt senden: <input type='checkbox' name='twice' value='1' ");
  if (cfg.sendTwice) page += F("checked");
  page += F("><br>");
  page += F("</fieldset><br>");

  page += F("<fieldset><legend>LAN Library (TCP)</legend>");
  page += F("ESP32 TCP Listen-Port:<br><input name='lanport' value='"); page += String(cfg.lanPort); page += F("'><br>");
  page += F("Keepalive (Sekunden, 0=aus):<br><input name='lankeep' value='"); page += String(cfg.lanKeepaliveSec); page += F("'><br>");
  page += F("<small>Rocrail verbindet sich zu diesem Port (ESP32 ist TCP-Server).</small>");
  page += F("</fieldset><br>");

  page += F("<fieldset><legend>MQTT</legend>");
  page += F("Broker Host:<br><input name='mqhost' value='"); page += htmlEscape(cfg.mqttHost); page += F("'><br>");
  page += F("Broker Port:<br><input name='mqport' value='"); page += String(cfg.mqttPort); page += F("'><br>");
  page += F("User:<br><input name='mquser' value='"); page += htmlEscape(cfg.mqttUser); page += F("'><br>");
  page += F("Pass:<br><input name='mqpass' type='password' value='"); page += htmlEscape(cfg.mqttPass); page += F("'><br>");
  page += F("Topic Base (default rocrail/service):<br><input name='mqbase' value='"); page += htmlEscape(cfg.mqttTopicBase); page += F("'><br>");
  page += F("QoS (0/1):<br><input name='mqqos' value='"); page += String(cfg.mqttQoS); page += F("'><br>");
  page += F("Keepalive (s):<br><input name='mqkeep' value='"); page += String(cfg.mqttKeepalive); page += F("'><br>");
  page += F("<small>Command Topic = &lt;base&gt;/command, Field Topic = &lt;base&gt;/field</small>");
  page += F("</fieldset><br>");

  page += F("<button type='submit'>Speichern & Neustart</button>");
  page += F("</form><hr>");

  page += F("<p>Status: ");
  page += (WiFi.status() == WL_CONNECTED) ? "STA verbunden" : "AP/Offline";
  page += F(" | Modus: ");
  page += modeName(cfg.ioMode);
  page += F("</p>");

  page += F("<hr><h3>Monitor Ausgabe</h3>");
  page += F("<div id='logs' style='background:#f0f0f0;padding:10px;height:300px;overflow-y:scroll;font-family:monospace;font-size:12px;white-space:pre-wrap;'>");
  page += F("Lade Logs...</div>");
  page += F("<script>");
  page += F("function fetchLogs(){fetch('/logs').then(r=>r.text()).then(d=>{document.getElementById('logs').innerHTML=d;})}");
  page += F("fetchLogs();setInterval(fetchLogs,2000);");
  page += F("</script>");

  page += F("</body></html>");
  server.send(200, "text/html", page);
}

static void handleSave() {
  saveConfigFromHttp();
  server.sendHeader("Location", "/");
  server.send(303, "text/plain", "Gespeichert. Neustart...");
  delay(500);
  ESP.restart();
}

static void handleLogs() {
  String response;
  int start = (logBufferCount < LOG_BUFFER_SIZE) ? 0 : logBufferIndex;
  for (int i = 0; i < logBufferCount; i++) {
    int idx = (start + i) % LOG_BUFFER_SIZE;
    response += logBuffer[idx] + "\n";
  }
  server.send(200, "text/plain", response);
}

static void startWeb() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/logs", HTTP_GET, handleLogs);
  server.begin();
  LOGI("WEB", "HTTP config server started on port 80");
}

// ----------------------------- Networking -----------------------------

static bool connectSta(uint32_t timeoutMs) {
  if (cfg.staSsid.length() == 0) return false;

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(cfg.staSsid.c_str(), cfg.staPass.c_str());

  LOGI("NET", "Connecting STA to SSID='%s' ...", cfg.staSsid.c_str());

  uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    if (WiFi.status() == WL_CONNECTED) {
      LOGI("NET", "STA connected, IP=%s, RSSI=%d dBm",
           WiFi.localIP().toString().c_str(), WiFi.RSSI());
      return true;
    }
    delay(250);
  }
  LOGW("NET", "STA connect timeout, fallback to AP");
  return false;
}

static void startFallbackAp() {
  WiFi.mode(WIFI_AP);
  String apName = "RocrailIO-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  bool ok = WiFi.softAP(apName.c_str());
  if (ok) {
    LOGI("NET", "AP started: SSID='%s', IP=%s",
         apName.c_str(), WiFi.softAPIP().toString().c_str());
  } else {
    LOGE("NET", "AP start failed");
  }
}

// ----------------------------- IO Setup -----------------------------

static void setupInputs() {
  int pins[16] = {0};
  bool inv[16] = {false};

  int nPins = splitCsvInt(cfg.inPinsCsv, pins, 16);
  int nInv  = splitCsvBool(cfg.inInvCsv, inv, 16);

  inCount = 0;
  for (int i = 0; i < nPins && inCount < 16; i++) {
    int p = pins[i];
    if (p < 0) continue;

    inStates[inCount].pin = p;
    inStates[inCount].invert = (i < nInv) ? inv[i] : false;

    pinMode(p, INPUT_PULLUP);

    bool raw = (digitalRead(p) != LOW);
    if (inStates[inCount].invert) raw = !raw;

    inStates[inCount].lastRaw = raw;
    inStates[inCount].stable = raw;
    inStates[inCount].lastChangeMs = millis();

    LOGI("IN", "Input[%u]: GPIO=%d invert=%d initial=%d addr=%u",
         (unsigned)inCount, p, inStates[inCount].invert ? 1 : 0,
         raw ? 1 : 0, (unsigned)(cfg.inAddrBase + inCount));

    inCount++;
  }

  LOGI("IN", "Inputs configured: %u (debounce=%u ms)", (unsigned)inCount, (unsigned)cfg.debounceMs);
}

static void setupOutputs() {
  int pins[16] = {0};
  bool inv[16] = {false};

  int nPins = splitCsvInt(cfg.outPinsCsv, pins, 16);
  int nInv  = splitCsvBool(cfg.outInvCsv, inv, 16);

  outCount = 0;
  for (int i = 0; i < nPins && outCount < 16; i++) {
    int p = pins[i];
    if (p < 0) continue;

    outStates[outCount].pin = p;
    outStates[outCount].invert = (i < nInv) ? inv[i] : false;
    outStates[outCount].logicalOn = false;

    pinMode(p, OUTPUT);

    bool phys = false;
    if (outStates[outCount].invert) phys = !phys;
    digitalWrite(p, phys ? HIGH : LOW);

    LOGI("OUT", "Output[%u]: GPIO=%d invert=%d initial=OFF maps to addr=%u port=%u",
         (unsigned)outCount, p, outStates[outCount].invert ? 1 : 0,
         (unsigned)cfg.outAddr, (unsigned)outCount);

    outCount++;
  }

  LOGI("OUT", "Outputs configured: %u (outAddr=%u)", (unsigned)outCount, (unsigned)cfg.outAddr);
}

static void setOutputPort(uint16_t port, bool on) {
  if (port >= outCount) {
    LOGW("OUT", "Port %u out of range (outCount=%u)", (unsigned)port, (unsigned)outCount);
    return;
  }

  outStates[port].logicalOn = on;

  bool phys = on;
  if (outStates[port].invert) phys = !phys;

  digitalWrite(outStates[port].pin, phys ? HIGH : LOW);
  LOGI("OUT", "Set port=%u GPIO=%d logical=%s phys=%s",
       (unsigned)port, outStates[port].pin,
       on ? "ON" : "OFF",
       phys ? "HIGH" : "LOW");
}

// ----------------------------- Common RCP handlers -----------------------------

static void handleRcpXml(const String& xml) {
  // IID filter if present
  String iid = getAttr(xml, "iid");
  if (iid.length() > 0 && iid != cfg.iid) {
    cntIidDrop++;
    LOGW("RCP", "Drop: iid mismatch (got='%s' expected='%s')", iid.c_str(), cfg.iid.c_str());
    return;
  }

  if (xml.startsWith("<co")) {
    String addrS = getAttr(xml, "addr");
    String portS = getAttr(xml, "port");
    String cmdS  = getAttr(xml, "cmd");
    if (addrS.length() == 0 || portS.length() == 0 || cmdS.length() == 0) {
      cntParseDrop++;
      LOGW("RCP", "Drop: malformed <co> (need addr/port/cmd) xml='%s'", xml.c_str());
      return;
    }

    uint16_t addr = (uint16_t)addrS.toInt();
    uint16_t port = (uint16_t)portS.toInt();
    if (addr != cfg.outAddr) {
      LOGW("OUT", "Ignore <co>: addr=%u != outAddr=%u", (unsigned)addr, (unsigned)cfg.outAddr);
      return;
    }

    bool on;
    if (!parseOnOff(cmdS, on)) {
      cntParseDrop++;
      LOGW("OUT", "Drop <co>: unknown cmd='%s'", cmdS.c_str());
      return;
    }

    cntCoCmds++;
    LOGI("OUT", "CO cmd: addr=%u port=%u cmd=%s", (unsigned)addr, (unsigned)port, on ? "on" : "off");
    setOutputPort(port, on);
    return;
  }

  LOGD("RCP", "RX ignored: %s", xml.c_str());
}

// ----------------------------- Transport: UUDP -----------------------------

static void uudpSendXml(const String& xml) {
  auto sendOnce = [&]() {
    if (!udp.beginPacket(cfg.rocHost.c_str(), cfg.rocPort)) {
      LOGW("UUDP", "beginPacket failed (host=%s port=%u)", cfg.rocHost.c_str(), cfg.rocPort);
      return;
    }
    udp.write((const uint8_t*)xml.c_str(), xml.length());
    udp.endPacket();
    cntTx++;
  };

  sendOnce();
  if (cfg.sendTwice) {
    delay(15);
    sendOnce();
  }

  LOGD("UUDP", "TX -> %s:%u %s", cfg.rocHost.c_str(), cfg.rocPort, xml.c_str());
}

static void uudpSendLogon() {
  uudpSendXml("<logon/>");
  lastLogonMs = millis();
  LOGI("UUDP", "Sent <logon/>");
}

static void uudpHandleIncoming() {
  int packetSize = udp.parsePacket();
  if (packetSize <= 0) return;

  char buf[512];
  int n = udp.read(buf, sizeof(buf) - 1);
  if (n <= 0) return;

  buf[n] = 0;
  String xml = String(buf);
  xml.trim();
  cntRx++;

  LOGD("UUDP", "RX <- %s:%u %s",
       udp.remoteIP().toString().c_str(), udp.remotePort(), xml.c_str());

  handleRcpXml(xml);
}

// ----------------------------- Transport: LAN Library (TCP Server) -----------------------------

static void lanStart() {
  lanServer = WiFiServer(cfg.lanPort);
  lanServer.begin();
  lanServer.setNoDelay(true);
  LOGI("LAN", "TCP server listening on port %u", (unsigned)cfg.lanPort);
}

static void lanStopClient() {
  if (lanClient && lanClient.connected()) {
    lanClient.stop();
  }
  lanRxBuf = "";
}

static void lanSendXml(const String& xml) {
  if (!lanClient || !lanClient.connected()) {
    LOGW("LAN", "TX skipped: no client connected");
    return;
  }
  // Rocrail LAN library uses plain XML strings; we terminate with newline for easy framing.
  lanClient.print(xml);
  lanClient.print("\n");
  lanClient.flush();
  cntTx++;
  LOGD("LAN", "TX -> %s", xml.c_str());
}

static void lanAcceptAndRead() {
  // Accept new client if none
  if (!lanClient || !lanClient.connected()) {
    WiFiClient newClient = lanServer.available();
    if (newClient) {
      lanClient = newClient;
      lanClient.setNoDelay(true);
      lanRxBuf = "";
      cntLanConn++;
      LOGI("LAN", "Client connected: %s:%u",
           lanClient.remoteIP().toString().c_str(), lanClient.remotePort());

      // Optional: announce presence (harmless for many setups)
      // You can comment this out if Rocrail LAN lib dislikes unsolicited data.
      lanSendXml("<logon/>");
    }
    return;
  }

  // Read bytes and split by newline (or '>' framing fallback)
  while (lanClient.available()) {
    char c = (char)lanClient.read();
    lanRxBuf += c;

    // Hard limit to avoid runaway
    if (lanRxBuf.length() > 1024) {
      LOGW("LAN", "RX buffer overflow, reset");
      lanRxBuf = "";
      cntParseDrop++;
      break;
    }

    // Prefer newline frame
    int nl = lanRxBuf.indexOf('\n');
    if (nl >= 0) {
      String line = lanRxBuf.substring(0, nl);
      lanRxBuf.remove(0, nl + 1);
      line.trim();
      if (line.length() == 0) continue;
      cntRx++;
      LOGD("LAN", "RX <- %s", line.c_str());
      handleRcpXml(line);
      continue;
    }

    // Fallback: if it looks like a single tag ended
    int gt = lanRxBuf.indexOf('>');
    if (gt >= 0 && lanRxBuf.startsWith("<")) {
      // may be multiple tags; handle first
      String one = lanRxBuf.substring(0, gt + 1);
      lanRxBuf.remove(0, gt + 1);
      one.trim();
      if (one.length() == 0) continue;
      cntRx++;
      LOGD("LAN", "RX <- %s", one.c_str());
      handleRcpXml(one);
    }
  }

  // Keepalive
  if (cfg.lanKeepaliveSec > 0) {
    uint32_t now = millis();
    if (now - lastLanKeepaliveMs >= (uint32_t)cfg.lanKeepaliveSec * 1000UL) {
      lastLanKeepaliveMs = now;
      lanSendXml("<ping/>");
      LOGD("LAN", "Sent <ping/>");
    }
  }

  // Disconnect detection
  if (lanClient && !lanClient.connected()) {
    cntLanDisc++;
    LOGW("LAN", "Client disconnected");
    lanStopClient();
  }
}

// ----------------------------- Transport: MQTT -----------------------------

static String mqttTopicCommand() { return cfg.mqttTopicBase + "/command"; }
static String mqttTopicField()   { return cfg.mqttTopicBase + "/field";   }

static void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String t(topic);
  String msg;
  msg.reserve(length + 1);
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  msg.trim();

  cntRx++;
  LOGD("MQTT", "RX topic=%s msg=%s", t.c_str(), msg.c_str());

  // We only handle command topic by default
  if (t == mqttTopicCommand()) {
    handleRcpXml(msg);
  } else {
    LOGD("MQTT", "Ignored topic=%s", t.c_str());
  }
}

static void mqttEnsureConnected() {
  if (mqtt.connected()) return;

  uint32_t now = millis();
  if (now - lastMqttReconnectMs < 3000UL) return;
  lastMqttReconnectMs = now;

  mqtt.setServer(cfg.mqttHost.c_str(), cfg.mqttPort);
  mqtt.setCallback(mqttCallback);
  mqtt.setKeepAlive(cfg.mqttKeepalive);

  String clientId = "rocrail-esp32-" + String((uint32_t)ESP.getEfuseMac(), HEX);

  LOGI("MQTT", "Connecting broker %s:%u as %s ...",
       cfg.mqttHost.c_str(), cfg.mqttPort, clientId.c_str());

  bool ok;
  if (cfg.mqttUser.length() > 0) {
    ok = mqtt.connect(clientId.c_str(), cfg.mqttUser.c_str(), cfg.mqttPass.c_str());
  } else {
    ok = mqtt.connect(clientId.c_str());
  }

  if (ok) {
    cntMqttConn++;
    LOGI("MQTT", "Connected. Subscribing: %s", mqttTopicCommand().c_str());
    mqtt.subscribe(mqttTopicCommand().c_str()); // PubSubClient QoS is internally 0/1, but API is simple
  } else {
    cntMqttDisc++;
    LOGW("MQTT", "Connect failed, rc=%d", mqtt.state());
  }
}

static void mqttSendXmlField(const String& xml) {
  if (!mqtt.connected()) {
    LOGW("MQTT", "TX skipped: not connected");
    return;
  }
  bool ok = mqtt.publish(mqttTopicField().c_str(), xml.c_str());
  if (ok) {
    cntTx++;
    LOGD("MQTT", "TX topic=%s msg=%s", mqttTopicField().c_str(), xml.c_str());
  } else {
    LOGW("MQTT", "Publish failed");
  }
}

// ----------------------------- Unified send for FB -----------------------------

static void sendFbEvent(uint16_t addr, bool state) {
  String xml = "<fb iid=\"" + cfg.iid + "\" addr=\"" + String(addr) +
               "\" state=\"" + (state ? "true" : "false") + "\"/>";

  switch (cfg.ioMode) {
    case MODE_UUDP:
      uudpSendXml(xml);
      break;
    case MODE_LAN:
      lanSendXml(xml);
      break;
    case MODE_MQTT:
      mqttSendXmlField(xml);
      break;
  }

  cntFbEvents++;
  LOGI("IN", "FB addr=%u state=%s", addr, state ? "true" : "false");
}

// ----------------------------- Runtime: Poll inputs -----------------------------

static void pollInputsAndSend() {
  const uint32_t now = millis();

  for (size_t i = 0; i < inCount; i++) {
    int p = inStates[i].pin;
    bool raw = (digitalRead(p) != LOW);
    if (inStates[i].invert) raw = !raw;

    if (raw != inStates[i].lastRaw) {
      inStates[i].lastRaw = raw;
      inStates[i].lastChangeMs = now;
      LOGD("IN", "GPIO=%d raw changed -> %d (debouncing)", p, raw ? 1 : 0);
    }

    if ((now - inStates[i].lastChangeMs) >= cfg.debounceMs) {
      if (inStates[i].stable != inStates[i].lastRaw) {
        inStates[i].stable = inStates[i].lastRaw;
        uint16_t addr = cfg.inAddrBase + (uint16_t)i;
        sendFbEvent(addr, inStates[i].stable);
      }
    }
  }
}

// ----------------------------- Status line -----------------------------

static void printStatusLine() {
  const bool sta = (WiFi.getMode() == WIFI_STA && WiFi.status() == WL_CONNECTED);
  String ip = sta ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  int rssi = sta ? WiFi.RSSI() : 0;

  LOGI("STAT",
       "mode=%s io=%s ip=%s rssi=%d heap=%u tx=%lu rx=%lu fb=%lu co=%lu drop=%lu iidDrop=%lu lanConn=%lu lanDisc=%lu mqConn=%lu mqDisc=%lu",
       sta ? "STA" : "AP",
       modeName(cfg.ioMode),
       ip.c_str(),
       rssi,
       (unsigned)ESP.getFreeHeap(),
       (unsigned long)cntTx,
       (unsigned long)cntRx,
       (unsigned long)cntFbEvents,
       (unsigned long)cntCoCmds,
       (unsigned long)cntParseDrop,
       (unsigned long)cntIidDrop,
       (unsigned long)cntLanConn,
       (unsigned long)cntLanDisc,
       (unsigned long)cntMqttConn,
       (unsigned long)cntMqttDisc);
}

// ----------------------------- Setup / Loop -----------------------------

void setup() {
  Serial.begin(115200);
  delay(200);

  LOGI("BOOT", "Starting ESP32 Rocrail IO (LOG_LEVEL=%d)", LOG_LEVEL);

  loadConfig();

  LOGI("CFG", "ioMode=%s iid=%s", modeName(cfg.ioMode), cfg.iid.c_str());
  LOGI("CFG", "inputs: base=%u pins='%s' inv='%s' deb=%u",
       cfg.inAddrBase, cfg.inPinsCsv.c_str(), cfg.inInvCsv.c_str(), cfg.debounceMs);
  LOGI("CFG", "outputs: addr=%u pins='%s' inv='%s'",
       cfg.outAddr, cfg.outPinsCsv.c_str(), cfg.outInvCsv.c_str());

  bool staOk = connectSta(15000);
  if (!staOk) startFallbackAp();

  startWeb();

  setupInputs();
  setupOutputs();

  // Init transport according to mode
  if (cfg.ioMode == MODE_UUDP) {
    if (!udp.begin(cfg.udpLocalPort)) {
      LOGE("UUDP", "udp.begin(%u) failed", (unsigned)cfg.udpLocalPort);
    } else {
      LOGI("UUDP", "Listening UDP on local port %u", (unsigned)cfg.udpLocalPort);
    }
    uudpSendLogon();
  } else if (cfg.ioMode == MODE_LAN) {
    lanStart();
  } else if (cfg.ioMode == MODE_MQTT) {
    mqtt.setBufferSize(1024);
    mqttEnsureConnected();
  }

  lastStatusMs = millis();
  printStatusLine();
}

void loop() {
  server.handleClient();

  // Mode-specific receive/maintenance
  if (cfg.ioMode == MODE_UUDP) {
    if (cfg.logonEverySec > 0) {
      uint32_t now = millis();
      if (now - lastLogonMs >= (uint32_t)cfg.logonEverySec * 1000UL) {
        uudpSendLogon();
      }
    }
    uudpHandleIncoming();
  } else if (cfg.ioMode == MODE_LAN) {
    lanAcceptAndRead();
  } else if (cfg.ioMode == MODE_MQTT) {
    mqttEnsureConnected();
    mqtt.loop();
  }

  pollInputsAndSend();

  // periodic status every 10s
  if (millis() - lastStatusMs >= 10000UL) {
    lastStatusMs = millis();
    printStatusLine();
  }

  delay(5);
}
