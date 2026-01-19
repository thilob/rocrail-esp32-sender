# ESP32 Rocrail IO Bridge (IR-Inputs & GPIO-Outputs)

## Zweck des Programms

Dieses Projekt macht aus einem **ESP32 DevKit** einen kleinen „I/O-Knoten“ für **Rocrail**:

- **Eingänge (Inputs):**  
  Der ESP32 liest **digitale Zustände** von IR-Reflexlichtschranken (Pin **DO** am Sensormodul) und sendet bei Zustandsänderungen ein Rocrail-Event:  
  `"<fb ... state='true/false'/>"`

- **Ausgänge (Outputs):**  
  Rocrail kann **Outputs** schalten, der ESP32 empfängt die entsprechenden Kommandos und setzt GPIOs (z. B. LED an GPIO25) über:  
  `"<co addr='...' port='...' cmd='on/off'/>"`

- **Transport/Protokoll (wählbar):**  
  In der Weboberfläche wählst du, wie die Kommunikation mit Rocrail laufen soll:
  1) **UUDP (UDP User Library)**  
  2) **LAN Library (TCP)**  
  3) **MQTT (Rocrail MQTT Service)**

Die Weboberfläche dient zur **Konfiguration ohne Programmierkenntnisse** (WLAN, Pins, Adressen, Protokollwahl) und enthält ein **Live-Monitoring** (Log-Ausgabe).

---

## Hardware – Kurzüberblick

### IR-Modul (4 Pins: VCC / GND / DO / AO)
Dieses Projekt nutzt **nur DO**.

**Wichtig (keine Zusatzbeschaltung gewünscht):**  
➡️ **IR-Modul an 3,3 V betreiben** (VCC → 3V3).  
So bleibt DO im sicheren Pegelbereich für ESP32-GPIOs.

**Verdrahtung (minimal):**
- IR **VCC** → ESP32 **3V3**
- IR **GND** → ESP32 **GND**
- IR **DO** → ESP32 **GPIOx** (konfigurierbar)
- IR **AO** → nicht verwenden

### LED an GPIO25
- GPIO25 → **Vorwiderstand 220–1000Ω** → LED Anode  
- LED Kathode → GND

> Der Vorwiderstand ist kein „Spannungsteiler“, sondern nötig, damit LED und ESP32 nicht beschädigt werden.

---

## Was steht in `platformio.ini` und wofür ist das?

Dieses Projekt wird typischerweise mit **PlatformIO (in VS Code)** gebaut.

Eine übliche `platformio.ini` sieht so aus:

```ini
[env:esp32dev]
platform = espressif32
board = esp32dev
framework = arduino
monitor_speed = 115200

lib_deps =
  knolleary/PubSubClient@^2.8
