# ESP32 Rocrail UUDP IO - Copilot Instructions

## Project Overview
ESP32 firmware bridging model railroad control (Rocrail/UUDP) with GPIO inputs/outputs. Acts as a remote IO node that receives/sends XML commands over UDP.

**Architecture**: Single-file monolithic design (`src/main.cpp`) with three main responsibilities:
1. **Web Config UI** (port 80) — WiFi credentials, IO pin mappings, Rocrail server address
2. **UUDP Client** (UDP bidirectional) — sends `<fb>` (feedback/input state) and receives `<co>` (command/output) XML
3. **GPIO I/O** — debounced inputs poll and trigger FB events; commands control output relays

## Critical Workflows

### Build & Flash
```
platformio run -e esp32dev -t upload     # Build & flash over USB
pio device monitor -b 115200              # View serial logs (LOG_LEVEL 0-3)
```

### Configuration Flow
1. **No STA wifi configured** → ESP32 starts AP (SSID: `RocrailIO-{MAC}`)
2. **Connect to AP** → visit `http://192.168.4.1` (ESP32 AP default IP)
3. **Fill form** (STA SSID, Rocrail host:port, GPIO pins, IID)
4. **Submit** → saves to NVS flash (`prefs.begin("rocrail")`), restarts
5. **Reconnects as STA** → resolves Rocrail host, begins listening UDP

### Logging Architecture
- **Real-time serial** (115200 baud) — all logs `[timestamp][level][tag] message`
- **Ring buffer** (`logBuffer[50]`) — web UI displays via `/logs` endpoint (auto-refresh every 2s)
- **Log levels**: E(0)=ERROR, W(1)=WARN, I(2)=INFO, D(3)=DEBUG
- **Control**: `#define LOG_LEVEL 2` compile-time; Serial.println debugging always works

## Code Organization & Patterns

### Configuration (Preferences)
- All settings stored via Arduino `Preferences` class (NVS flash)
- Namespace: `"rocrail"` — survives restarts
- Types: String, int (via toInt()), bool
- Example: `prefs.getString("ssid", "")` with fallback defaults
- Modification: user edits web form → `saveConfigFromHttp()` → `prefs.put*()` → ESP restart

### GPIO Pin Management
**Inputs** (`inStates[]` array):
- Read with `INPUT_PULLUP` (IR sensor active-low typical)
- CSV config: pins `"16,17,18,19"` → addresses base+index
- Per-pin invert flag (`inInvCsv`) normalizes active logic
- Debounce: 40ms default, rejects noise before triggering FB event
- Poll in `pollInputsAndSend()` (called in loop, ~5ms cadence)

**Outputs** (`outStates[]` array):
- Write with `OUTPUT`, default LOW (OFF)
- Matched by `<co addr=X port=Y cmd=on/off>` where addr=`cfg.outAddr`, port=array index
- Per-pin invert flag inverts physical GPIO before digitalWrite

### UUDP XML Messaging
**Sending** (ESP → Rocrail):
- Logon: `<logon/>` (at startup, optionally periodic every N seconds)
- Feedback: `<fb iid="esp32" addr="105" state="true"/>` (input state change)
- Sent twice by default (redundancy) with 15ms gap

**Receiving** (Rocrail → ESP):
- Output command: `<co iid="esp32" addr="1" port="2" cmd="on"/>`
- Parsed by simple string search (`xml.indexOf`, `getAttr()` helper)
- Filters: `iid` mismatch → dropped; `addr` != `cfg.outAddr` → ignored
- Malformed XML (`cmd` not on/off/1/0/true/false) → dropped with counter

### Naming Conventions
- **Addresses**: Rocrail's sensor/switch address (0-65535 range typical)
  - Inputs: base + index (e.g., base=100 → sensors 100,101,102,...)
  - Outputs: single addr matching all ports (e.g., addr=1, ports 0,1,2,...)
- **Log tags**: `"IN"`, `"OUT"`, `"UUDP"`, `"NET"`, `"WEB"`, `"CFG"`, `"BOOT"`
- **State variables**: `lastRaw` (physical), `stable` (debounced)

## Common Modifications

### Add New Config Parameter
1. Add field to `struct Config { ... };`
2. Load in `loadConfig()`: `cfg.newField = prefs.getString("key", "default");`
3. Save in `saveConfigFromHttp()`: `prefs.putString("key", server.arg("key"));`
4. Add input in web form HTML (in `handleRoot()`)
5. Use in logic (e.g., `if (cfg.newField == ...) ...`)

### Adjust Debounce or Add Input Sensitivity
- Edit `cfg.debounceMs` default in `loadConfig()` or via web UI
- Logic in `pollInputsAndSend()`: waits `debounceMs` after raw change before stable edge
- Reduce for responsive sensors; increase for noisy environments

### Extend XML Parsing
- New command tags: add `if (xml.startsWith("<newtag"))` block in `handleIncomingUdp()`
- Use `getAttr(xml, "attrname")` to extract XML attributes
- Log & count unhandled messages to aid debugging

## Dependencies & Environment
- **Platform**: PlatformIO + Arduino ESP32 core
- **Board**: esp32dev (generic ESP32-DEVKIT-V1)
- **Libs**: WiFi, WebServer, Preferences (all built-in Arduino framework)
- **Serial baud**: 115200
- **Monitor command**: `pio device monitor -b 115200` (shows real-time logs)

## Testing & Debugging
- **Manual UDP test**: `echo '<co addr="1" port="0" cmd="on"/>' | nc -u 192.168.1.X 21112`
- **WiFi issues**: Check STA logs → falls back to AP; visit AP IP if stuck
- **GPIO not responding**: Verify pin CSV syntax (no spaces after commas); check invert flags
- **Lost logs**: Ring buffer overwrites oldest; refresh `/logs` page to see latest
- **Stuck config**: Hold GPIO to force AP mode (not implemented—requires code addition)

## Key Files
- [platformio.ini](../platformio.ini) — board, framework, baud rate config
- [src/main.cpp](../src/main.cpp) — entire application (654 lines, well-commented sections)

---
*Last updated: January 2025*
