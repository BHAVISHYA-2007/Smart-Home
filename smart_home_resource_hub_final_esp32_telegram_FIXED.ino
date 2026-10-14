/*
  Smart Home Resource Hub - ESP32 DevKit firmware
  Designed for smart_home_hub_donezo_dark final.html

  Hardware map (adjust if your wiring differs):
    DHT11 DATA -> GPIO13
    PIR OUT -> GPIO27
    LDR analog AO -> GPIO34 (must stay <= 3.3V)
    RGB common-cathode: R=GPIO21, G=GPIO22, B=GPIO19 (each through 220-330 ohm)
    Active buzzer signal -> GPIO25
    HC-SR04 TRIG -> GPIO26, ECHO -> GPIO33 THROUGH A VOLTAGE DIVIDER
    ST7735 SPI: SCK=18, MOSI/SDA=23, CS=5, DC=4, RST=2
  IMPORTANT:
    - ESP32 GPIO pins are NOT 5V tolerant.
    - Do not connect AC mains or a pump/fan directly to ESP32 pins.
    - Fan and pump are reported unavailable because no output pin/driver is configured.
    - Energy and water flow are null until compatible sensors are integrated.
    - Telegram uses HTTPS with certificate verification disabled for simplicity.
      Use a secure certificate strategy before deploying beyond a personal prototype.
    - ArduinoJson v7 (or compatible v6), DHT sensor library, Adafruit GFX and
      Adafruit ST7735/ST7789 libraries are required.
*/

#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <DHT.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SPI.h>
#include <time.h>

// ---------------- USER CONFIG ----------------
const char* WIFI_SSID = "Platinum";
const char* WIFI_PASSWORD = "Jimmy@123";
const char* TELEGRAM_BOT_TOKEN = "8784525743:AAFZPqMJux8vPh-Ir0ITGStAF1BsxHX6XC4";
// Use your private Telegram numeric chat ID. Bot responds only to this chat.
const char* TELEGRAM_ALLOWED_CHAT_ID = "6704291028";
const bool TELEGRAM_ENABLED = true;
// ---------------------------------------------

// Pins
static const uint8_t DHT_PIN = 13;
static const uint8_t PIR_PIN = 27;
static const uint8_t LDR_PIN = 34;
static const uint8_t RGB_R_PIN = 21;
static const uint8_t RGB_G_PIN = 22;
static const uint8_t RGB_B_PIN = 19;
static const uint8_t BUZZER_PIN = 25;
static const uint8_t TRIG_PIN = 26;
static const uint8_t ECHO_PIN = 33;
static const uint8_t TFT_CS = 5;
static const uint8_t TFT_DC = 4;
static const uint8_t TFT_RST = 2;

#define DHT_TYPE DHT11
DHT dht(DHT_PIN, DHT_TYPE);
Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_RST);
WebServer server(80);
Preferences prefs;

float temperatureC = NAN, humidityPct = NAN, distanceCm = NAN;
float waterLevelCm = NAN, waterLevelPct = NAN;
int ldrRaw = -1;
bool pirMotion = false, roomDark = false;
bool buzzerMuted = false, buzzerActive = false;
bool dhtOK = false, distanceOK = false;
bool telegramReady = false;
String ledMode = "auto";
String ledState = "OFF";
float tempThreshold = 32.0f;
float objectThreshold = 20.0f;
float containerHeight = 25.0f;
int ldrDarkThreshold = 1800;
bool notifyMotion = true, notifyNear = true, notifyTemperature = true;
bool notifyWater = true, notifySensorErrors = true;
bool dailySummaryEnabled = false;
int summaryHour = 20;
int lastSummaryDay = -1;
unsigned long lastSensorRead = 0, lastTftUpdate = 0, lastTelegramPoll = 0;
unsigned long lastAlertMotion = 0, lastAlertNear = 0, lastAlertTemp = 0;
unsigned long lastAlertWater = 0, lastAlertSensor = 0;
unsigned long telegramOffset = 0;
bool prevMotion = false, prevNear = false, prevHighTemp = false;
bool prevWaterLow = false, prevWaterHigh = false, prevDhtError = false;
String lastTelegramError = "Not checked";

const unsigned long SENSOR_INTERVAL_MS = 2000;
const unsigned long TELEGRAM_POLL_MS = 1200;
const unsigned long ALERT_COOLDOWN_MS = 60000;

String encodeUrl(const String &s) {
  String out;
  const char *hex = "0123456789ABCDEF";
  for (size_t i = 0; i < s.length(); i++) {
    uint8_t c = (uint8_t)s[i];
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
      out += (char)c;
    } else if (c == ' ') out += "%20";
    else {
      out += '%'; out += hex[c >> 4]; out += hex[c & 15];
    }
  }
  return out;
}

bool telegramConfigured() {
  return TELEGRAM_ENABLED &&
    String(TELEGRAM_BOT_TOKEN).length() > 20 &&
    String(TELEGRAM_BOT_TOKEN).indexOf("PASTE_") < 0 &&
    String(TELEGRAM_ALLOWED_CHAT_ID).length() > 3 &&
    String(TELEGRAM_ALLOWED_CHAT_ID).indexOf("PASTE_") < 0;
}

void telegramSend(const String &message) {
  if (!telegramConfigured() || WiFi.status() != WL_CONNECTED) return;
  WiFiClientSecure client;
  client.setInsecure(); // Prototype convenience; see security note above.
  HTTPClient http;
  String url = "https://api.telegram.org/bot" + String(TELEGRAM_BOT_TOKEN) + "/sendMessage";
  if (!http.begin(client, url)) { lastTelegramError = "Could not start HTTPS request"; return; }
  http.addHeader("Content-Type", "application/json");
  JsonDocument doc;
  doc["chat_id"] = TELEGRAM_ALLOWED_CHAT_ID;
  doc["text"] = message;
  doc["disable_web_page_preview"] = true;
  String body; serializeJson(doc, body);
  int code = http.POST(body);
  lastTelegramError = code >= 200 && code < 300 ? "OK" : "sendMessage HTTP " + String(code);
  http.end();
}

String n1(float v, uint8_t decimals = 1) {
  if (!isfinite(v)) return "Unavailable";
  // Cast decimals explicitly to avoid an ambiguous String(float, integer) overload
  // with some ESP32 Arduino core versions.
  return String(v, (unsigned int)decimals);
}
String boolWord(bool v) { return v ? "YES" : "NO"; }

void setRgbRaw(bool r, bool g, bool b) {
  // Common-cathode LED: HIGH turns a colour on. If yours is common-anode,
  // invert these three output levels.
  digitalWrite(RGB_R_PIN, r ? HIGH : LOW);
  digitalWrite(RGB_G_PIN, g ? HIGH : LOW);
  digitalWrite(RGB_B_PIN, b ? HIGH : LOW);
}
void setLedMode(const String &mode) {
  if (mode != "auto" && mode != "red" && mode != "yellow" &&
      mode != "green" && mode != "blue" && mode != "off") return;
  ledMode = mode;
}
void applyLed() {
  if (ledMode == "red") { setRgbRaw(true, false, false); ledState = "RED"; return; }
  if (ledMode == "yellow") { setRgbRaw(true, true, false); ledState = "YELLOW"; return; }
  if (ledMode == "green") { setRgbRaw(false, true, false); ledState = "GREEN"; return; }
  if (ledMode == "blue") { setRgbRaw(false, false, true); ledState = "BLUE"; return; }
  if (ledMode == "off") { setRgbRaw(false, false, false); ledState = "OFF"; return; }

  if (isfinite(temperatureC) && temperatureC > tempThreshold) {
    setRgbRaw(true, false, false); ledState = "RED";
  } else if (isfinite(distanceCm) && distanceCm > 1 && distanceCm <= objectThreshold) {
    setRgbRaw(true, false, false); ledState = "RED";
  } else if (isfinite(distanceCm) && distanceCm > objectThreshold && distanceCm <= 50) {
    setRgbRaw(true, true, false); ledState = "YELLOW";
  } else if (pirMotion && roomDark) {
    setRgbRaw(false, false, true); ledState = "BLUE";
  } else {
    setRgbRaw(false, true, false); ledState = "GREEN";
  }
}

void updateBuzzer() {
  bool highTemp = isfinite(temperatureC) && temperatureC > tempThreshold;
  bool near = isfinite(distanceCm) && distanceCm > 1 && distanceCm <= objectThreshold;
  buzzerActive = !buzzerMuted && (highTemp || near);
  digitalWrite(BUZZER_PIN, buzzerActive ? HIGH : LOW);
}

float readDistance() {
  digitalWrite(TRIG_PIN, LOW); delayMicroseconds(3);
  digitalWrite(TRIG_PIN, HIGH); delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  unsigned long duration = pulseIn(ECHO_PIN, HIGH, 30000UL);
  if (duration == 0) return NAN;
  float d = duration * 0.0343f / 2.0f;
  if (d < 2 || d > 400) return NAN;
  return d;
}

void loadSettings() {
  prefs.begin("smarthome", false);
  tempThreshold = prefs.getFloat("tempLimit", 32.0f);
  objectThreshold = prefs.getFloat("nearLimit", 20.0f);
  containerHeight = prefs.getFloat("tankHeight", 25.0f);
  ldrDarkThreshold = prefs.getInt("ldrDark", 1800);
  buzzerMuted = prefs.getBool("buzzMute", false);
  String m = prefs.getString("ledMode", "auto");
  if (m == "auto" || m == "red" || m == "yellow" || m == "green" || m == "blue" || m == "off") ledMode = m;
  notifyMotion = prefs.getBool("nMotion", true);
  notifyNear = prefs.getBool("nNear", true);
  notifyTemperature = prefs.getBool("nTemp", true);
  notifyWater = prefs.getBool("nWater", true);
  notifySensorErrors = prefs.getBool("nSensor", true);
  dailySummaryEnabled = prefs.getBool("daily", false);
  summaryHour = prefs.getInt("sumHour", 20);
}
void saveFloat(const char *key, float value) { prefs.putFloat(key, value); }
void saveBool(const char *key, bool value) { prefs.putBool(key, value); }

void readSensors() {
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  dhtOK = isfinite(t) && isfinite(h) && h >= 0 && h <= 100;
  temperatureC = dhtOK ? t : NAN;
  humidityPct = dhtOK ? h : NAN;
  pirMotion = digitalRead(PIR_PIN) == HIGH;
  ldrRaw = analogRead(LDR_PIN);
  roomDark = ldrRaw >= 0 && ldrRaw < ldrDarkThreshold;
  distanceCm = readDistance();
  distanceOK = isfinite(distanceCm);
  if (distanceOK) {
    waterLevelCm = constrain(containerHeight - distanceCm, 0.0f, containerHeight);
    waterLevelPct = constrain((waterLevelCm / containerHeight) * 100.0f, 0.0f, 100.0f);
  } else {
    waterLevelCm = NAN; waterLevelPct = NAN;
  }
  applyLed();
  updateBuzzer();
}

void addCors() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
  server.sendHeader("Cache-Control", "no-store");
}
void handleOptions() { addCors(); server.send(204); }

void handleStatus() {
  addCors();
  JsonDocument d;
  d["online"] = WiFi.status() == WL_CONNECTED;
  d["ip"] = WiFi.localIP().toString();
  if (isfinite(temperatureC)) d["temperature"] = temperatureC; else d["temperature"] = nullptr;
  if (isfinite(humidityPct)) d["humidity"] = humidityPct; else d["humidity"] = nullptr;
  if (isfinite(distanceCm)) d["distanceCm"] = distanceCm; else d["distanceCm"] = nullptr;
  if (isfinite(waterLevelCm)) d["waterLevelCm"] = waterLevelCm; else d["waterLevelCm"] = nullptr;
  if (isfinite(waterLevelPct)) d["waterLevelPct"] = (int)roundf(waterLevelPct); else d["waterLevelPct"] = nullptr;
  d["ldrRaw"] = ldrRaw >= 0 ? ldrRaw : -1;
  d["roomDark"] = roomDark;
  d["pir"] = pirMotion;
  d["sensorOK"] = dhtOK;
  d["distanceOK"] = distanceOK;
  d["highTemp"] = isfinite(temperatureC) && temperatureC > tempThreshold;
  d["nearObject"] = isfinite(distanceCm) && distanceCm > 1 && distanceCm <= objectThreshold;
  d["led"] = ledState;
  d["ledMode"] = ledMode;
  d["buzzer"] = buzzerActive;
  d["buzzerMuted"] = buzzerMuted;
  d["tempThreshold"] = tempThreshold;
  d["objectThreshold"] = objectThreshold;
  d["containerHeight"] = containerHeight;
  d["powerW"] = nullptr;
  d["energyWh"] = nullptr;
  d["waterFlowLpm"] = nullptr;
  d["fanAvailable"] = false;
  d["waterAvailable"] = false;
  d["telegramEnabled"] = telegramConfigured();
  d["telegramStatus"] = lastTelegramError;
  String out; serializeJson(d, out); server.send(200, "application/json", out);
}

void handleControl() {
  addCors();
  bool changed = false;
  String msg = "No supported control parameter supplied.";
  if (server.hasArg("ledMode")) {
    String m = server.arg("ledMode"); m.toLowerCase();
    if (m == "auto" || m == "red" || m == "yellow" || m == "green" || m == "blue" || m == "off") {
      setLedMode(m); prefs.putString("ledMode", ledMode); changed = true; msg = "LED mode set to " + ledMode;
    } else { server.send(400, "application/json", "{\"ok\":false,\"error\":\"Invalid ledMode\"}"); return; }
  }
  if (server.hasArg("buzzerMute")) {
    String v = server.arg("buzzerMute");
    if (v == "1" || v == "true") buzzerMuted = true;
    else if (v == "0" || v == "false") buzzerMuted = false;
    else { server.send(400, "application/json", "{\"ok\":false,\"error\":\"buzzerMute must be 0 or 1\"}"); return; }
    saveBool("buzzMute", buzzerMuted); updateBuzzer(); changed = true; msg = buzzerMuted ? "Buzzer muted" : "Buzzer unmuted";
  }
  if (server.hasArg("tempThreshold")) {
    float v = server.arg("tempThreshold").toFloat();
    if (v < 10 || v > 70) { server.send(400, "application/json", "{\"ok\":false,\"error\":\"tempThreshold must be 10-70 C\"}"); return; }
    tempThreshold = v; saveFloat("tempLimit", v); changed = true; msg = "Temperature threshold saved";
  }
  if (server.hasArg("objectThreshold")) {
    float v = server.arg("objectThreshold").toFloat();
    if (v < 2 || v > 400) { server.send(400, "application/json", "{\"ok\":false,\"error\":\"objectThreshold must be 2-400 cm\"}"); return; }
    objectThreshold = v; saveFloat("nearLimit", v); changed = true; msg = "Near-object threshold saved";
  }
  if (server.hasArg("containerHeight")) {
    float v = server.arg("containerHeight").toFloat();
    if (v < 5 || v > 400) { server.send(400, "application/json", "{\"ok\":false,\"error\":\"containerHeight must be 5-400 cm\"}"); return; }
    containerHeight = v; saveFloat("tankHeight", v); changed = true; msg = "Container height saved";
  }
  if (!changed) { server.send(400, "application/json", "{\"ok\":false,\"error\":\"No supported parameter\"}"); return; }
  applyLed(); updateBuzzer();
  JsonDocument d; d["ok"] = true; d["message"] = msg;
  String out; serializeJson(d, out); server.send(200, "application/json", out);
}

String liveStatusText() {
  String s = "🏠 Smart Home Resource Hub\n";
  s += "Connection: " + String(WiFi.status() == WL_CONNECTED ? "Online" : "Offline") + "\n";
  s += "IP: " + WiFi.localIP().toString() + "\n\n";
  s += "🌡 Temperature: " + n1(temperatureC) + (isfinite(temperatureC) ? " °C" : "") + "\n";
  s += "💧 Humidity: " + n1(humidityPct, 0) + (isfinite(humidityPct) ? "%" : "") + "\n";
  s += "📏 Distance: " + n1(distanceCm) + (isfinite(distanceCm) ? " cm" : "") + "\n";
  s += "🚰 Estimated water level: " + n1(waterLevelPct, 0) + (isfinite(waterLevelPct) ? "%" : "") + "\n";
  s += "   Estimated height: " + n1(waterLevelCm) + (isfinite(waterLevelCm) ? " cm" : "") + "\n";
  s += "🕯 Room: " + String(roomDark ? "Dark" : "Bright") + " (LDR raw " + String(ldrRaw) + ")\n";
  s += "🚶 Motion: " + String(pirMotion ? "Detected" : "No motion") + "\n";
  s += "💡 RGB LED: " + ledState + " (mode " + ledMode + ")\n";
  s += "🔔 Buzzer: " + String(buzzerMuted ? "Muted" : buzzerActive ? "ALERT ON" : "Off") + "\n";
  s += "🌡 Alert threshold: " + String(tempThreshold, 1) + " °C\n";
  s += "📐 Near-object threshold: " + String(objectThreshold, 1) + " cm\n";
  s += "📦 Container height: " + String(containerHeight, 1) + " cm\n";
  s += "⚡ Energy: unavailable (no energy sensor integrated)\n";
  s += "🌊 Water flow: unavailable (no flow sensor integrated)\n";
  s += "🌀 Fan / pump: unavailable (driver not wired)\n";
  return s;
}

String helpText() {
  return "🏠 SmartHome Assistant\n\n"
    "Live data:\n/status - all readings\n/temp - temperature\n/humidity - humidity\n/water - estimated tank level\n/distance - ultrasonic distance\n/motion - PIR status\n/light - LDR brightness\n/devices - RGB/buzzer/output status\n/diagnostics - sensor & network health\n\n"
    "Controls:\n/led auto|red|yellow|green|blue|off\n/mute or /unmute\n/threshold 32\n/near 20\n/tankheight 25\n\n"
    "Alerts:\n/alerts on|off - all optional alerts\n/notify motion|near|temp|water|sensor on|off\n/daily on|off - daily summary\n/summaryhour 20 - 0 to 23\n\n"
    "Other:\n/summary - current report\n/help - command list\n\n"
    "Fan/pump, energy and water flow are unavailable until hardware is added.";
}

void sendSensorReply(const String &cmd) {
  if (cmd == "/status" || cmd == "/summary") { telegramSend(liveStatusText()); return; }
  if (cmd == "/temp" || cmd.indexOf("temperature") >= 0 || cmd.indexOf("how hot") >= 0) {
    telegramSend("🌡 Temperature: " + n1(temperatureC) + (isfinite(temperatureC) ? " °C" : "") +
      "\nAlert threshold: " + String(tempThreshold, 1) + " °C\n" +
      (isfinite(temperatureC) ? (temperatureC > tempThreshold ? "⚠️ Above threshold." : "Within configured threshold.") : "Check DHT11 wiring."));
    return;
  }
  if (cmd == "/humidity" || cmd.indexOf("humidity") >= 0) { telegramSend("💧 Humidity: " + n1(humidityPct, 0) + (isfinite(humidityPct) ? "%" : "") + (dhtOK ? "" : "\nDHT11 reading unavailable.")); return; }
  if (cmd == "/water" || cmd.indexOf("water level") >= 0 || cmd.indexOf("tank") >= 0) {
    telegramSend("🚰 Estimated tank level: " + n1(waterLevelPct, 0) + (isfinite(waterLevelPct) ? "%" : "") +
      "\nEstimated water height: " + n1(waterLevelCm) + (isfinite(waterLevelCm) ? " cm" : "") +
      "\nSensor distance: " + n1(distanceCm) + (isfinite(distanceCm) ? " cm" : "") +
      "\nNote: assumes the ultrasonic sensor faces downward from the tank top; percentage is height-based, not volume.");
    return;
  }
  if (cmd == "/distance" || cmd.indexOf("distance") >= 0) { telegramSend("📏 HC-SR04 distance: " + n1(distanceCm) + (isfinite(distanceCm) ? " cm" : "\nNo echo. Check TRIG/ECHO wiring and the ECHO voltage divider.")); return; }
  if (cmd == "/motion" || cmd.indexOf("motion") >= 0 || cmd.indexOf("movement") >= 0) { telegramSend(String("🚶 Motion: ") + (pirMotion ? "DETECTED" : "No motion") + "\nPIR GPIO: " + String(PIR_PIN)); return; }
  if (cmd == "/light" || cmd.indexOf("dark") >= 0 || cmd.indexOf("brightness") >= 0 || cmd.indexOf("light") >= 0) { telegramSend("💡 Room: " + String(roomDark ? "Dark" : "Bright") + "\nLDR raw ADC: " + String(ldrRaw) + "\nDark threshold: " + String(ldrDarkThreshold) + "\nNote: raw direction depends on the LDR divider wiring."); return; }
  if (cmd == "/devices" || cmd.indexOf("device status") >= 0 || cmd.indexOf("devices") >= 0) {
    telegramSend("Devices:\nRGB LED: " + ledState + " (mode " + ledMode + ")\nBuzzer: " + String(buzzerMuted ? "Muted" : buzzerActive ? "Alert ON" : "Off") +
      "\nFan/relay: unavailable\nWater pump: unavailable\nEnergy sensor: unavailable\nWater-flow sensor: unavailable");
    return;
  }
  if (cmd == "/diagnostics") {
    telegramSend("🩺 Diagnostics\nWi-Fi: " + String(WiFi.status() == WL_CONNECTED ? "connected" : "disconnected") +
      "\nIP: " + WiFi.localIP().toString() + "\nRSSI: " + String(WiFi.RSSI()) + " dBm\nDHT11: " + String(dhtOK ? "OK" : "ERROR") +
      "\nHC-SR04: " + String(distanceOK ? "OK" : "NO ECHO") + "\nPIR: GPIO" + String(PIR_PIN) +
      "\nLDR ADC: " + String(ldrRaw) + "\nTelegram: " + lastTelegramError);
    return;
  }
}

bool parseOnOff(String v, bool &out) {
  v.toLowerCase();
  if (v == "on" || v == "1" || v == "true" || v == "enable" || v == "enabled") { out = true; return true; }
  if (v == "off" || v == "0" || v == "false" || v == "disable" || v == "disabled") { out = false; return true; }
  return false;
}

String normalizeCommand(String text) {
  text.trim(); text.toLowerCase();
  int at = text.indexOf('@');
  if (at >= 0) text = text.substring(0, at);
  while (text.startsWith(" ")) text.remove(0, 1);
  return text;
}

void processTelegramMessage(String chatId, String text) {
  if (String(TELEGRAM_ALLOWED_CHAT_ID) != chatId) {
    // Do not respond to unknown chats; avoids exposing home telemetry.
    return;
  }
  String cmd = normalizeCommand(text);
  if (cmd.length() == 0) return;
  if (cmd == "/start" || cmd == "/help" || cmd == "help" || cmd == "what can you do") { telegramSend(helpText()); return; }
  if (cmd == "hi" || cmd == "hello" || cmd == "hey" || cmd == "good morning" || cmd == "good evening") {
    telegramSend("Hello! 👋 I'm your SmartHome Resource Hub assistant.\nSend /status for live readings or /help to see what I can do."); return;
  }

  if (cmd.startsWith("/led")) {
    int sp = cmd.indexOf(' '); String mode = sp >= 0 ? cmd.substring(sp + 1) : "";
    mode.trim();
    if (mode != "auto" && mode != "red" && mode != "yellow" && mode != "green" && mode != "blue" && mode != "off") {
      telegramSend("Usage: /led auto|red|yellow|green|blue|off"); return;
    }
    setLedMode(mode); prefs.putString("ledMode", ledMode); applyLed();
    telegramSend("RGB LED mode changed to " + ledMode + "."); return;
  }
  if (cmd == "/mute") { buzzerMuted = true; saveBool("buzzMute", true); updateBuzzer(); telegramSend("🔕 Buzzer muted. Monitoring and Telegram alerts remain active."); return; }
  if (cmd == "/unmute") { buzzerMuted = false; saveBool("buzzMute", false); updateBuzzer(); telegramSend("🔔 Buzzer unmuted."); return; }

  if (cmd.startsWith("/threshold")) {
    int sp = cmd.indexOf(' '); if (sp < 0) { telegramSend("Usage: /threshold 32 (valid range 10-70 °C)"); return; }
    float v = cmd.substring(sp + 1).toFloat();
    if (v < 10 || v > 70) { telegramSend("Temperature threshold must be 10-70 °C."); return; }
    tempThreshold = v; saveFloat("tempLimit", v); updateBuzzer(); applyLed();
    telegramSend("🌡 High-temperature threshold saved: " + String(v, 1) + " °C."); return;
  }
  if (cmd.startsWith("/near")) {
    int sp = cmd.indexOf(' '); if (sp < 0) { telegramSend("Usage: /near 20 (valid range 2-400 cm)"); return; }
    float v = cmd.substring(sp + 1).toFloat();
    if (v < 2 || v > 400) { telegramSend("Near-object threshold must be 2-400 cm."); return; }
    objectThreshold = v; saveFloat("nearLimit", v); updateBuzzer(); applyLed();
    telegramSend("📏 Near-object threshold saved: " + String(v, 1) + " cm."); return;
  }
  if (cmd.startsWith("/tankheight")) {
    int sp = cmd.indexOf(' '); if (sp < 0) { telegramSend("Usage: /tankheight 25 (valid range 5-400 cm)"); return; }
    float v = cmd.substring(sp + 1).toFloat();
    if (v < 5 || v > 400) { telegramSend("Tank height must be 5-400 cm."); return; }
    containerHeight = v; saveFloat("tankHeight", v);
    telegramSend("📦 Tank reference height saved: " + String(v, 1) + " cm."); return;
  }

  if (cmd.startsWith("/alerts")) {
    int sp = cmd.indexOf(' '); bool v;
    if (sp < 0 || !parseOnOff(cmd.substring(sp + 1), v)) { telegramSend("Usage: /alerts on|off"); return; }
    notifyMotion = notifyNear = notifyTemperature = notifyWater = notifySensorErrors = v;
    saveBool("nMotion", v); saveBool("nNear", v); saveBool("nTemp", v); saveBool("nWater", v); saveBool("nSensor", v);
    telegramSend(String("All optional Telegram alerts ") + (v ? "enabled." : "disabled.")); return;
  }
  if (cmd.startsWith("/notify ")) {
    String args = cmd.substring(8); args.trim(); int sp = args.indexOf(' ');
    if (sp < 0) { telegramSend("Usage: /notify motion|near|temp|water|sensor on|off"); return; }
    String key = args.substring(0, sp); String val = args.substring(sp + 1); bool v;
    if (!parseOnOff(val, v)) { telegramSend("Use on or off."); return; }
    if (key == "motion") { notifyMotion = v; saveBool("nMotion", v); }
    else if (key == "near") { notifyNear = v; saveBool("nNear", v); }
    else if (key == "temp") { notifyTemperature = v; saveBool("nTemp", v); }
    else if (key == "water") { notifyWater = v; saveBool("nWater", v); }
    else if (key == "sensor") { notifySensorErrors = v; saveBool("nSensor", v); }
    else { telegramSend("Unknown alert category. Use motion, near, temp, water or sensor."); return; }
    telegramSend("Alert preference saved: " + key + " = " + String(v ? "on" : "off")); return;
  }
  if (cmd.startsWith("/daily")) {
    int sp = cmd.indexOf(' '); bool v;
    if (sp < 0 || !parseOnOff(cmd.substring(sp + 1), v)) { telegramSend("Usage: /daily on|off"); return; }
    dailySummaryEnabled = v; saveBool("daily", v);
    telegramSend(String("Daily summary ") + (v ? "enabled" : "disabled") + ". Set the hour with /summaryhour 20."); return;
  }
  if (cmd.startsWith("/summaryhour")) {
    int sp = cmd.indexOf(' '); if (sp < 0) { telegramSend("Usage: /summaryhour 20 (0-23)"); return; }
    int h = cmd.substring(sp + 1).toInt();
    if (h < 0 || h > 23) { telegramSend("Hour must be 0-23. ESP32 clock uses network time when available."); return; }
    summaryHour = h; prefs.putInt("sumHour", h); telegramSend("Daily summary hour set to " + String(h) + ":00 local ESP32 time."); return;
  }

  // Natural-language shortcuts
  if (cmd.indexOf("status") >= 0 || cmd.indexOf("everything") >= 0 || cmd.indexOf("report") >= 0) { sendSensorReply("/status"); return; }
  if (cmd.indexOf("temperature") >= 0 || cmd.indexOf("how hot") >= 0 || cmd.indexOf("hot") >= 0) { sendSensorReply("/temp"); return; }
  if (cmd.indexOf("humidity") >= 0 || cmd.indexOf("humid") >= 0) { sendSensorReply("/humidity"); return; }
  if (cmd.indexOf("water") >= 0 || cmd.indexOf("tank") >= 0) { sendSensorReply("/water"); return; }
  if (cmd.indexOf("distance") >= 0 || cmd.indexOf("near") >= 0) { sendSensorReply("/distance"); return; }
  if (cmd.indexOf("motion") >= 0 || cmd.indexOf("movement") >= 0 || cmd.indexOf("someone") >= 0) { sendSensorReply("/motion"); return; }
  if (cmd.indexOf("light") >= 0 || cmd.indexOf("dark") >= 0 || cmd.indexOf("bright") >= 0) { sendSensorReply("/light"); return; }
  if (cmd.indexOf("device") >= 0 || cmd.indexOf("fan") >= 0 || cmd.indexOf("pump") >= 0) { sendSensorReply("/devices"); return; }
  if (cmd.indexOf("diagnostic") >= 0 || cmd.indexOf("sensor error") >= 0) { sendSensorReply("/diagnostics"); return; }
  telegramSend("I don't recognise that request yet.\nTry /status, /temp, /water, /motion, /devices or /help.");
}

void pollTelegram() {
  if (!telegramConfigured() || WiFi.status() != WL_CONNECTED) return;
  WiFiClientSecure client;
  client.setInsecure(); // Prototype convenience; see security note above.
  HTTPClient http;
  String url = "https://api.telegram.org/bot" + String(TELEGRAM_BOT_TOKEN) +
               "/getUpdates?offset=" + String(telegramOffset) + "&timeout=0&allowed_updates=%5B%22message%22%5D";
  if (!http.begin(client, url)) { lastTelegramError = "getUpdates begin failed"; return; }
  http.setTimeout(3500);
  int code = http.GET();
  if (code != 200) {
    lastTelegramError = "getUpdates HTTP " + String(code);
    http.end(); return;
  }
  String payload = http.getString(); http.end();
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) { lastTelegramError = "getUpdates JSON parse error"; return; }
  if (!doc["ok"].as<bool>()) { lastTelegramError = "Telegram API returned ok=false"; return; }
  JsonArray results = doc["result"].as<JsonArray>();
  for (JsonObject update : results) {
    unsigned long updateId = update["update_id"] | 0UL;
    if (updateId >= telegramOffset) telegramOffset = updateId + 1;
    JsonObject msg = update["message"];
    if (msg.isNull()) continue;
    String chatId = msg["chat"]["id"].as<String>();
    String text = msg["text"] | "";
    if (text.length()) processTelegramMessage(chatId, text);
  }
  lastTelegramError = "OK";
}

void checkAutomaticAlerts() {
  unsigned long now = millis();
  bool near = isfinite(distanceCm) && distanceCm > 1 && distanceCm <= objectThreshold;
  bool highTemp = isfinite(temperatureC) && temperatureC > tempThreshold;
  bool lowWater = isfinite(waterLevelPct) && waterLevelPct <= 15;
  bool highWater = isfinite(waterLevelPct) && waterLevelPct >= 95;
  bool dhtError = !dhtOK;

  if (pirMotion && !prevMotion && notifyMotion && now - lastAlertMotion > ALERT_COOLDOWN_MS) {
    telegramSend("🚶 Motion detected by PIR sensor.\n" + liveStatusText()); lastAlertMotion = now;
  }
  if (near && !prevNear && notifyNear && now - lastAlertNear > ALERT_COOLDOWN_MS) {
    telegramSend("⚠️ Nearby object detected: " + n1(distanceCm) + " cm (threshold " + String(objectThreshold, 1) + " cm)."); lastAlertNear = now;
  }
  if (highTemp && !prevHighTemp && notifyTemperature && now - lastAlertTemp > ALERT_COOLDOWN_MS) {
    telegramSend("🔥 High temperature alert: " + n1(temperatureC) + " °C, threshold " + String(tempThreshold, 1) + " °C."); lastAlertTemp = now;
  }
  if ((lowWater && !prevWaterLow) && notifyWater && now - lastAlertWater > ALERT_COOLDOWN_MS) {
    telegramSend("🚰 Low estimated water level: " + n1(waterLevelPct, 0) + "%."); lastAlertWater = now;
  }
  if ((highWater && !prevWaterHigh) && notifyWater && now - lastAlertWater > ALERT_COOLDOWN_MS) {
    telegramSend("🚰 Tank estimated nearly full: " + n1(waterLevelPct, 0) + "%."); lastAlertWater = now;
  }
  if (dhtError && !prevDhtError && notifySensorErrors && now - lastAlertSensor > ALERT_COOLDOWN_MS) {
    telegramSend("⚠️ DHT11 readings unavailable. Check sensor power, data pin and library."); lastAlertSensor = now;
  }
  prevMotion = pirMotion; prevNear = near; prevHighTemp = highTemp;
  prevWaterLow = lowWater; prevWaterHigh = highWater; prevDhtError = dhtError;
}

void checkDailySummary() {
  if (!dailySummaryEnabled || WiFi.status() != WL_CONNECTED) return;
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 10)) return;
  if (timeinfo.tm_hour == summaryHour && timeinfo.tm_mday != lastSummaryDay) {
    telegramSend("🗓 Daily SmartHome summary\n" + liveStatusText());
    lastSummaryDay = timeinfo.tm_mday;
  }
}

// ---------- Modern compact TFT dashboard (ST7735 160x128 landscape) ----------
uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
const uint16_t UI_BG      = 0x0861; // deep blue-black
const uint16_t UI_PANEL   = 0x10A3;
const uint16_t UI_PANEL2  = 0x18E5;
const uint16_t UI_MUTED   = 0x9D13;
const uint16_t UI_WHITE   = 0xFFFF;
const uint16_t UI_MINT    = 0x36D7;
const uint16_t UI_CYAN    = 0x2DDF;
const uint16_t UI_PURPLE  = 0xB39F;
const uint16_t UI_ORANGE  = 0xFD20;
const uint16_t UI_RED     = 0xF986;
const uint16_t UI_LINE    = 0x2968;
unsigned long lastTftPageChange = 0;
uint8_t tftPage = 0;

void tftText(int16_t x, int16_t y, const String &s, uint16_t color, uint8_t size=1) {
  tft.setTextColor(color); tft.setTextSize(size); tft.setCursor(x, y); tft.print(s);
}
void tftHeader(const char *section, uint8_t page) {
  tft.fillScreen(UI_BG);
  // Small brand mark and title
  tft.fillRoundRect(5, 5, 18, 18, 5, UI_MINT);
  tft.drawLine(10, 14, 14, 9, UI_BG);
  tft.drawLine(14, 9, 18, 14, UI_BG);
  tft.drawRect(11, 13, 7, 7, UI_BG);
  tftText(29, 6, "SMART HOME", UI_WHITE, 1);
  tftText(29, 16, section, UI_MUTED, 1);
  // Network status chip
  uint16_t statusColor = WiFi.status() == WL_CONNECTED ? UI_MINT : UI_RED;
  tft.fillRoundRect(119, 7, 36, 15, 7, UI_PANEL2);
  tft.fillCircle(125, 14, 2, statusColor);
  tftText(130, 10, WiFi.status() == WL_CONNECTED ? "ON" : "OFF", statusColor, 1);
  tft.drawFastHLine(5, 28, 150, UI_LINE);
  // Two page position dots
  tft.fillCircle(145, 121, 2, page == 0 ? UI_MINT : UI_LINE);
  tft.fillCircle(152, 121, 2, page == 1 ? UI_MINT : UI_LINE);
}
void tftMetricCard(int16_t x, int16_t y, int16_t w, int16_t h,
                   const char *label, const String &value, const char *unit,
                   uint16_t accent) {
  tft.fillRoundRect(x, y, w, h, 7, UI_PANEL);
  tft.fillRoundRect(x, y, 3, h, 2, accent);
  tftText(x+8, y+5, label, UI_MUTED, 1);
  tftText(x+8, y+17, value, UI_WHITE, 2);
  int16_t unitX = x + 8 + (int16_t)value.length() * 12;
  if (unitX < x+w-18) tftText(unitX, y+23, unit, accent, 1);
}
void tftProgress(int16_t x, int16_t y, int16_t w, int value, uint16_t color) {
  value = constrain(value, 0, 100);
  tft.fillRoundRect(x, y, w, 5, 2, UI_PANEL2);
  if (value > 0) tft.fillRoundRect(x, y, max(3, (w*value)/100), 5, 2, color);
}
void drawTftOverview() {
  tftHeader("LIVE OVERVIEW", 0);
  String temp = isfinite(temperatureC) ? String(temperatureC, 1) : "--";
  String hum  = isfinite(humidityPct) ? String((int)roundf(humidityPct)) : "--";
  tftMetricCard(5, 34, 73, 43, "TEMPERATURE", temp, "C", UI_ORANGE);
  tftMetricCard(82, 34, 73, 43, "HUMIDITY", hum, "%", UI_CYAN);

  // Water level card with compact progress indicator
  tft.fillRoundRect(5, 82, 93, 31, 7, UI_PANEL);
  tftText(11, 87, "WATER LEVEL", UI_MUTED, 1);
  String water = isfinite(waterLevelPct) ? String((int)roundf(waterLevelPct)) + "%" : "--";
  tftText(11, 99, water, UI_WHITE, 1);
  tftProgress(47, 103, 43, isfinite(waterLevelPct) ? (int)waterLevelPct : 0, UI_CYAN);

  // Proximity card
  tft.fillRoundRect(103, 82, 52, 31, 7, UI_PANEL);
  tftText(109, 87, "DIST", UI_MUTED, 1);
  String dist = isfinite(distanceCm) ? String(distanceCm, 0) : "--";
  tftText(109, 99, dist, UI_PURPLE, 2);
  tftText(135, 103, "cm", UI_MUTED, 1);

  tftText(6, 117, dhtOK ? "SENSORS OK" : "CHECK DHT11", dhtOK ? UI_MINT : UI_RED, 1);
}
void drawTftStatus() {
  tftHeader("HOME STATUS", 1);
  // Motion state
  tft.fillRoundRect(5, 34, 72, 30, 7, UI_PANEL);
  tftText(11, 39, "MOTION", UI_MUTED, 1);
  uint16_t motionColor = pirMotion ? UI_ORANGE : UI_MINT;
  tft.fillCircle(14, 55, 3, motionColor);
  tftText(22, 50, pirMotion ? "DETECTED" : "CLEAR", motionColor, 1);

  // Light state
  tft.fillRoundRect(82, 34, 73, 30, 7, UI_PANEL);
  tftText(88, 39, "AMBIENT", UI_MUTED, 1);
  tftText(88, 50, roomDark ? "DARK" : "BRIGHT", roomDark ? UI_PURPLE : UI_ORANGE, 1);

  // Output status and threshold info
  tft.fillRoundRect(5, 69, 150, 25, 7, UI_PANEL);
  tftText(11, 74, "RGB", UI_MUTED, 1);
  tftText(35, 74, ledState, UI_MINT, 1);
  tftText(91, 74, "BUZZER", UI_MUTED, 1);
  tftText(91, 84, buzzerMuted ? "MUTED" : buzzerActive ? "ALERT" : "OFF",
          buzzerActive ? UI_RED : UI_CYAN, 1);

  tft.fillRoundRect(5, 99, 150, 17, 6, UI_PANEL2);
  tftText(10, 103, "TEMP LIMIT", UI_MUTED, 1);
  tftText(70, 103, String(tempThreshold, 0) + " C", UI_ORANGE, 1);
  tftText(108, 103, "NEAR " + String(objectThreshold, 0), UI_PURPLE, 1);
  tftText(6, 118, WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "Wi-Fi disconnected", 
          WiFi.status() == WL_CONNECTED ? UI_MINT : UI_RED, 1);
}
void updateTft() {
  unsigned long now = millis();
  if (now - lastTftPageChange >= 5000) {
    lastTftPageChange = now;
    tftPage = (tftPage + 1) % 2;
    lastTftUpdate = 0; // force redraw on page change
  }
  if (now - lastTftUpdate < 1200) return;
  lastTftUpdate = now;
  tft.setTextWrap(false);
  if (tftPage == 0) drawTftOverview();
  else drawTftStatus();
}

void handleRoot() {
  addCors();
  server.send(200, "text/plain", "Smart Home Resource Hub ESP32 API\nGET /api/status\nGET /api/control?ledMode=green\nGET /api/control?buzzerMute=1\n");
}

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to Wi-Fi");
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 25000) {
    delay(350); Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Wi-Fi connected. IP: "); Serial.println(WiFi.localIP());
    configTime(19800, 0, "pool.ntp.org", "time.google.com", "time.nist.gov"); // India UTC+5:30
  } else Serial.println("Wi-Fi not connected. Check 2.4GHz SSID/password.");
}

void setup() {
  Serial.begin(115200);
  delay(200);
  pinMode(PIR_PIN, INPUT);
  pinMode(RGB_R_PIN, OUTPUT); pinMode(RGB_G_PIN, OUTPUT); pinMode(RGB_B_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(TRIG_PIN, OUTPUT); pinMode(ECHO_PIN, INPUT);
  digitalWrite(TRIG_PIN, LOW); digitalWrite(BUZZER_PIN, LOW);
  analogReadResolution(12);
  dht.begin();
  loadSettings();

  SPI.begin(18, -1, 23, TFT_CS);
  tft.initR(INITR_BLACKTAB); // If colours/offset are wrong, try INITR_GREENTAB or INITR_REDTAB.
  tft.setRotation(1);
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextColor(ST77XX_CYAN); tft.setTextSize(1);
  tft.setCursor(5, 10); tft.print("SMART HOME HUB");
  tft.setTextColor(ST77XX_WHITE); tft.setCursor(5, 28); tft.print("Starting...");

  connectWiFi();

  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/status", HTTP_OPTIONS, handleOptions);
  server.on("/api/control", HTTP_GET, handleControl);
  server.on("/api/control", HTTP_OPTIONS, handleOptions);
  server.onNotFound([]() { addCors(); server.send(404, "application/json", "{\"error\":\"Not found\"}"); });
  server.begin();
  Serial.println("HTTP server started on port 80.");
  Serial.println("Status endpoint: http://" + WiFi.localIP().toString() + "/api/status");
  Serial.println(telegramConfigured() ? "Telegram interactive assistant configured." : "Telegram placeholders not configured; fill token/chat ID.");
  if (telegramConfigured()) telegramSend("🏠 SmartHome Resource Hub is online.\nSend /start or /help to begin.");
}

void loop() {
  server.handleClient();
  if (WiFi.status() != WL_CONNECTED) {
    static unsigned long lastReconnect = 0;
    if (millis() - lastReconnect > 10000) { lastReconnect = millis(); WiFi.reconnect(); }
  }
  if (millis() - lastSensorRead >= SENSOR_INTERVAL_MS) {
    lastSensorRead = millis();
    readSensors();
    checkAutomaticAlerts();
  }
  if (millis() - lastTelegramPoll >= TELEGRAM_POLL_MS) {
    lastTelegramPoll = millis();
    pollTelegram();
  }
  checkDailySummary();
  updateTft();
  delay(2);
}
