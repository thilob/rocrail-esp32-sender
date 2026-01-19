#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <WiFiUdp.h>

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
static WiFiUDP udp;

struct Config {
  // WiFi STA
  String staSsid;
  String staPass;

  // Rocrail UUDP remote
  String rocHost;     // IP/DNS
  uint16_t rocPort;   // usually 21111
  String iid;         // e.g. "esp32"

  // Inputs -> <fb addr=...>
  uint16_t inAddrBase; // addr = base + index
  String inPinsCsv;    // e.g. "16,17,18,19"
  String inInvCsv;     // e.g. "0,0,0,0"
  uint16_t debounceMs;

  // UDP behavior
  uint16_t udpLocalPort;   // where we listen for incoming commands
  bool sendTwice;          // send redundancy
  uint16_t logonEverySec;  // 0=only once

  // Outputs -> <co addr=... port=... cmd=on/off>
  uint16_t outAddr;
  String outPinsCsv;
  String outInvCsv;
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

// Counters
static uint32_t cntUdpTx = 0;
static uint32_t cntUdpRx = 0;
static uint32_t cntFbEvents = 0;
static uint32_t cntCoCmds = 0;
static uint32_t cntParseDrop = 0;
static uint32_t cntIidDrop = 0;

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

// ----------------------------- Config -----------------------------

static void loadConfig() {
  prefs.begin("rocrail", true);

  cfg.staSsid = prefs.getString("ssid", "");
  cfg.staPass = prefs.getString("pass", "");

  cfg.rocHost = prefs.getString("host", "192.168.1.10");
  cfg.rocPort = (uint16_t)prefs.getUShort("rport", 21111);
  cfg.iid     = prefs.getString("iid", "esp32");

  cfg.inAddrBase  = (uint16_t)prefs.getUShort("ibase", 100);
  cfg.inPinsCsv   = prefs.getString("ipins", DEFAULT_INPUT_PINS);
  cfg.inInvCsv    = prefs.getString("iinv",  "0,0,0,0");
  cfg.debounceMs  = (uint16_t)prefs.getUShort("deb", 40);

  cfg.udpLocalPort  = (uint16_t)prefs.getUShort("lport", 21112);
  cfg.sendTwice     = prefs.getBool("twice", true);
  cfg.logonEverySec = (uint16_t)prefs.getUShort("logsec", 60);

  cfg.outAddr     = (uint16_t)prefs.getUShort("oaddr", 1);
  cfg.outPinsCsv  = prefs.getString("opins", DEFAULT_OUTPUT_PINS);
  cfg.outInvCsv   = prefs.getString("oinv",  "0,0,0,0");

  prefs.end();
}

static void saveConfigFromHttp() {
  prefs.begin("rocrail", false);

  prefs.putString("ssid", server.arg("ssid"));
  prefs.putString("pass", server.arg("pass"));

  prefs.putString("host", server.arg("host"));
  prefs.putUShort("rport", (uint16_t)server.arg("rport").toInt());
  prefs.putString("iid", server.arg("iid"));

  prefs.putUShort("ibase", (uint16_t)server.arg("ibase").toInt());
  prefs.putString("ipins", server.arg("ipins"));
  prefs.putString("iinv",  server.arg("iinv"));
  prefs.putUShort("deb", (uint16_t)server.arg("deb").toInt());

  prefs.putUShort("lport", (uint16_t)server.arg("lport").toInt());
  prefs.putBool("twice", server.arg("twice") == "1");
  prefs.putUShort("logsec", (uint16_t)server.arg("logsec").toInt());

  prefs.putUShort("oaddr", (uint16_t)server.arg("oaddr").toInt());
  prefs.putString("opins", server.arg("opins"));
  prefs.putString("oinv",  server.arg("oinv"));

  prefs.end();
}

// ----------------------------- Web UI -----------------------------

static void handleRoot() {
  String page; page.reserve(3200);
  page += F("<!doctype html><html><head><meta charset='utf-8'>"
            "<meta name='viewport' content='width=device-width,initial-scale=1'>"
            "<title>ESP32 Rocrail UUDP IO</title></head><body>");
  page += F("<h2>ESP32 Rocrail UUDP IO</h2>");

  page += F("<form method='POST' action='/save'>");

  page += F("<fieldset><legend>WLAN (STA)</legend>");
  page += F("SSID:<br><input name='ssid' value='"); page += htmlEscape(cfg.staSsid); page += F("'><br>");
  page += F("Passwort:<br><input name='pass' type='password' value='"); page += htmlEscape(cfg.staPass); page += F("'><br>");
  page += F("</fieldset><br>");

  page += F("<fieldset><legend>Rocrail UUDP (Remote)</legend>");
  page += F("Rocrail Host/IP:<br><input name='host' value='"); page += htmlEscape(cfg.rocHost); page += F("'><br>");
  page += F("Rocrail UDP Port:<br><input name='rport' value='"); page += String(cfg.rocPort); page += F("'><br>");
  page += F("IID (z.B. esp32):<br><input name='iid' value='"); page += htmlEscape(cfg.iid); page += F("'><br>");
  page += F("</fieldset><br>");

  page += F("<fieldset><legend>UDP Local</legend>");
  page += F("Local UDP Port (listen):<br><input name='lport' value='"); page += String(cfg.udpLocalPort); page += F("'><br>");
  page += F("Logon alle X Sekunden (0 = nur beim Start):<br><input name='logsec' value='"); page += String(cfg.logonEverySec); page += F("'><br>");
  page += F("UDP doppelt senden: <input type='checkbox' name='twice' value='1' ");
  if (cfg.sendTwice) page += F("checked");
  page += F("><br>");
  page += F("</fieldset><br>");

  page += F("<fieldset><legend>Inputs (IR DO)</legend>");
  page += F("Addr-Base (addr = base + index):<br><input name='ibase' value='"); page += String(cfg.inAddrBase); page += F("'><br>");
  page += F("Input GPIOs CSV:<br><input name='ipins' value='"); page += htmlEscape(cfg.inPinsCsv); page += F("'><br>");
  page += F("Invert CSV (0/1 passend zu Inputs):<br><input name='iinv' value='"); page += htmlEscape(cfg.inInvCsv); page += F("'><br>");
  page += F("Debounce ms:<br><input name='deb' value='"); page += String(cfg.debounceMs); page += F("'><br>");
  page += F("</fieldset><br>");

  page += F("<fieldset><legend>Outputs (Rocrail &lt;co&gt;)</legend>");
  page += F("Output addr (match &lt;co addr='...'>):<br><input name='oaddr' value='"); page += String(cfg.outAddr); page += F("'><br>");
  page += F("Output GPIOs CSV (Index = port):<br><input name='opins' value='"); page += htmlEscape(cfg.outPinsCsv); page += F("'><br>");
  page += F("Invert CSV (0/1 passend zu Outputs):<br><input name='oinv' value='"); page += htmlEscape(cfg.outInvCsv); page += F("'><br>");
  page += F("</fieldset><br>");

  page += F("<button type='submit'>Speichern & Neustart</button>");
  page += F("</form><hr>");

  page += F("<p>Status: ");
  page += (WiFi.status() == WL_CONNECTED) ? "STA verbunden" : "AP/Offline";
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
  String response = "";
  // Logs in richtiger Reihenfolge ausgeben (älteste zuerst)
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

static void udpSendXml(const String& xml) {
  auto sendOnce = [&]() {
    if (!udp.beginPacket(cfg.rocHost.c_str(), cfg.rocPort)) {
      LOGW("UUDP", "beginPacket failed (host=%s port=%u)", cfg.rocHost.c_str(), cfg.rocPort);
      return;
    }
    udp.write((const uint8_t*)xml.c_str(), xml.length());
    udp.endPacket();
    cntUdpTx++;
  };

  sendOnce();
  if (cfg.sendTwice) {
    delay(15);
    sendOnce();
  }

  LOGD("UUDP", "TX -> %s:%u  %s", cfg.rocHost.c_str(), cfg.rocPort, xml.c_str());
}

static void sendLogon() {
  udpSendXml("<logon/>");
  lastLogonMs = millis();
  LOGI("UUDP", "Sent <logon/>");
}

static void sendFbEvent(uint16_t addr, bool state) {
  String xml = "<fb iid=\"" + cfg.iid + "\" addr=\"" + String(addr) +
               "\" state=\"" + (state ? "true" : "false") + "\"/>";
  udpSendXml(xml);
  cntFbEvents++;
  LOGI("IN", "FB addr=%u state=%s", addr, state ? "true" : "false");
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

    // DO von deinem IR-Modul: ideal mit INPUT_PULLUP, Modul liefert häufig low/high je nach Trigger
    pinMode(p, INPUT_PULLUP);

    bool raw = (digitalRead(p) != LOW); // HIGH=true
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

    // initial OFF
    bool phys = false;
    if (outStates[outCount].invert) phys = !phys;
    digitalWrite(p, phys ? HIGH : LOW);

    LOGI("OUT", "Output[%u]: GPIO=%d invert=%d initial=OFF  maps to <co addr=%u port=%u>",
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

// ----------------------------- Runtime: Handle incoming UDP -----------------------------

static void handleIncomingUdp() {
  int packetSize = udp.parsePacket();
  if (packetSize <= 0) return;

  char buf[512];
  int n = udp.read(buf, sizeof(buf) - 1);
  if (n <= 0) return;

  buf[n] = 0;
  String xml = String(buf);
  xml.trim();
  cntUdpRx++;

  LOGD("UUDP", "RX <- %s:%u  %s",
       udp.remoteIP().toString().c_str(), udp.remotePort(), xml.c_str());

  // IID filter (wenn iid enthalten und != cfg.iid -> drop)
  String iid = getAttr(xml, "iid");
  if (iid.length() > 0 && iid != cfg.iid) {
    cntIidDrop++;
    LOGW("UUDP", "Drop: iid mismatch (got='%s' expected='%s')", iid.c_str(), cfg.iid.c_str());
    return;
  }

  // Output command: <co addr="X" port="Y" cmd="on|off|..."/>
  if (xml.startsWith("<co")) {
    String addrS = getAttr(xml, "addr");
    String portS = getAttr(xml, "port");
    String cmdS  = getAttr(xml, "cmd");
    if (addrS.length() == 0 || portS.length() == 0 || cmdS.length() == 0) {
      cntParseDrop++;
      LOGW("UUDP", "Drop: malformed <co> (need addr/port/cmd)");
      return;
    }

    uint16_t addr = (uint16_t)addrS.toInt();
    uint16_t port = (uint16_t)portS.toInt();
    if (addr != cfg.outAddr) {
      LOGW("OUT", "Ignore <co>: addr=%u != outAddr=%u", (unsigned)addr, (unsigned)cfg.outAddr);
      return;
    }

    bool on;
    if (cmdS == "on" || cmdS == "1" || cmdS == "true") on = true;
    else if (cmdS == "off" || cmdS == "0" || cmdS == "false") on = false;
    else {
      // unknown cmd
      cntParseDrop++;
      LOGW("OUT", "Drop <co>: unknown cmd='%s'", cmdS.c_str());
      return;
    }

    cntCoCmds++;
    LOGI("OUT", "CO cmd: addr=%u port=%u cmd=%s", (unsigned)addr, (unsigned)port, on ? "on" : "off");
    setOutputPort(port, on);
    return;
  }

  // andere Tags derzeit ignorieren
  LOGD("UUDP", "RX ignored (tag not handled)");
}

// ----------------------------- Status line -----------------------------

static void printStatusLine() {
  const bool sta = (WiFi.getMode() == WIFI_STA && WiFi.status() == WL_CONNECTED);
  String ip = sta ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  int rssi = sta ? WiFi.RSSI() : 0;

  LOGI("STAT",
       "mode=%s ip=%s rssi=%d heap=%u tx=%lu rx=%lu fb=%lu co=%lu drop=%lu iidDrop=%lu",
       sta ? "STA" : "AP",
       ip.c_str(),
       rssi,
       (unsigned)ESP.getFreeHeap(),
       (unsigned long)cntUdpTx,
       (unsigned long)cntUdpRx,
       (unsigned long)cntFbEvents,
       (unsigned long)cntCoCmds,
       (unsigned long)cntParseDrop,
       (unsigned long)cntIidDrop);
}

// ----------------------------- Setup / Loop -----------------------------

void setup() {
  Serial.begin(115200);
  delay(200);

  LOGI("BOOT", "Starting ESP32 Rocrail UUDP IO (LOG_LEVEL=%d)", LOG_LEVEL);

  loadConfig();

  LOGI("CFG", "roc=%s:%u iid=%s lport=%u twice=%d logsec=%u",
       cfg.rocHost.c_str(), cfg.rocPort, cfg.iid.c_str(), cfg.udpLocalPort,
       cfg.sendTwice ? 1 : 0, cfg.logonEverySec);

  LOGI("CFG", "inputs: base=%u pins='%s' inv='%s' deb=%u",
       cfg.inAddrBase, cfg.inPinsCsv.c_str(), cfg.inInvCsv.c_str(), cfg.debounceMs);

  LOGI("CFG", "outputs: addr=%u pins='%s' inv='%s'",
       cfg.outAddr, cfg.outPinsCsv.c_str(), cfg.outInvCsv.c_str());

  bool staOk = connectSta(15000);
  if (!staOk) startFallbackAp();

  startWeb();

  // Start UDP listener on fixed local port (needed for incoming <co>)
  if (!udp.begin(cfg.udpLocalPort)) {
    LOGE("UUDP", "udp.begin(%u) failed", (unsigned)cfg.udpLocalPort);
  } else {
    LOGI("UUDP", "Listening UDP on local port %u", (unsigned)cfg.udpLocalPort);
  }

  setupInputs();
  setupOutputs();

  sendLogon();
  lastStatusMs = millis();
  printStatusLine();
}

void loop() {
  server.handleClient();

  // Optional periodic logon
  if (cfg.logonEverySec > 0) {
    uint32_t now = millis();
    if (now - lastLogonMs >= (uint32_t)cfg.logonEverySec * 1000UL) {
      sendLogon();
    }
  }

  handleIncomingUdp();
  pollInputsAndSend();

  // periodic status every 10s
  if (millis() - lastStatusMs >= 10000UL) {
    lastStatusMs = millis();
    printStatusLine();
  }

  delay(5);
}
