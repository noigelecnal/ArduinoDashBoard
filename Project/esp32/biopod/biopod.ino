/*
  BioPod ESP32 firmware (Arduino)
  Reads 4x AM2302B (DHT22), runs on/off control with a latching hysteresis band,
  and POSTs timestamped JSON to the dashboard.

  Spec source: BioPod capstone paper
    - Setpoints: 37 C, 50 % RH (defaults; the dashboard can change them)
    - Acceptance range: +/-0.5 C, +/-5 % RH
    - Hysteresis band: +/-0.2 C (Objective 5)
    - Blower runs continuously; Peltier engages only above the upper acceptance limit

  Libraries: "DHT sensor library" (Adafruit) + "Adafruit Unified Sensor".
  Dashboard: run `python server.py`, then set DASHBOARD_HOST to this PC's LAN IP.

  NOTE: pin numbers below must be checked against the 4.0-inch ESP32 touchscreen
  board you actually use; the display and touch controller occupy several GPIOs.
*/

#include <WiFi.h>
#include <HTTPClient.h>
#include <time.h>
#include "DHT.h"

// ---------- Network ----------
const char* WIFI_SSID = "YOUR_WIFI";
const char* WIFI_PASS = "YOUR_PASSWORD";
const char* DASHBOARD_HOST = "192.168.1.10";
const uint16_t DASHBOARD_PORT = 8080;

// ---------- Control parameters ----------
float tempSetpoint = 37.0;               // default, overwritten by dashboard value
float humSetpoint = 50.0;                // default, overwritten by dashboard value
const float TEMP_ACCEPT = 0.5;           // acceptance range (+/- C)
const float TEMP_HYST = 0.2;             // hysteresis band (+/- C)
const float HUM_ACCEPT = 5.0;            // acceptance range (+/- % RH)
const float HUM_HYST = 2.0;              // humidifier hysteresis (+/- % RH)

const float TEMP_SP_MIN = 25.0, TEMP_SP_MAX = 45.0;   // safety clamp for remote setpoints
const float HUM_SP_MIN = 20.0, HUM_SP_MAX = 90.0;

const unsigned long SAMPLE_INTERVAL_MS = 5000;        // DHT22 needs >= 2 s
const unsigned long WIFI_RETRY_MS = 10000;

// ---------- Pins ----------
#define DHTTYPE DHT22
const int DHT_PINS[4] = {14, 27, 26, 25};
const int PIN_HEATER = 32;
const int PIN_PELTIER = 33;
const int PIN_FAN = 18;
const int PIN_HUMIDIFIER = 19;

DHT dht0(DHT_PINS[0], DHTTYPE);
DHT dht1(DHT_PINS[1], DHTTYPE);
DHT dht2(DHT_PINS[2], DHTTYPE);
DHT dht3(DHT_PINS[3], DHTTYPE);
DHT* sensors[4] = {&dht0, &dht1, &dht2, &dht3};

bool heaterOn = false;
bool peltierOn = false;
bool humidifierOn = false;

unsigned long lastSample = 0;
unsigned long lastWifiTry = 0;

String numOrNull(float v) {
  if (isnan(v)) return "null";
  return String(v, 2);
}

// Pull a number out of a tiny JSON string, e.g. "temp":37.0
bool extractNumber(const String& body, const char* key, float& out) {
  String needle = String("\"") + key + "\":";
  int idx = body.indexOf(needle);
  if (idx < 0) return false;
  out = body.substring(idx + needle.length()).toFloat();
  return true;
}

void applyServerSetpoints(const String& resp) {
  int sp = resp.indexOf("\"setpoints\"");
  if (sp < 0) return;
  String part = resp.substring(sp);
  float t, h;
  if (extractNumber(part, "temp", t) && t >= TEMP_SP_MIN && t <= TEMP_SP_MAX) tempSetpoint = t;
  if (extractNumber(part, "hum", h) && h >= HUM_SP_MIN && h <= HUM_SP_MAX) humSetpoint = h;
}

// Latching on/off control (state is remembered between samples = real hysteresis)
void controlTemperature(float avgT) {
  if (isnan(avgT)) {            // no valid sensor: fail safe, everything off
    heaterOn = false;
    peltierOn = false;
    return;
  }
  float heatOn = tempSetpoint - TEMP_HYST;
  float heatOff = tempSetpoint + TEMP_HYST;
  float coolOn = tempSetpoint + TEMP_ACCEPT;   // only above the upper acceptance limit
  float coolOff = tempSetpoint + TEMP_HYST;

  if (!heaterOn && avgT < heatOn) heaterOn = true;
  else if (heaterOn && avgT >= heatOff) heaterOn = false;

  if (!peltierOn && avgT > coolOn) peltierOn = true;
  else if (peltierOn && avgT <= coolOff) peltierOn = false;

  if (heaterOn && peltierOn) heaterOn = false;  // never heat and cool together
}

void controlHumidity(float avgH) {
  if (isnan(avgH)) {
    humidifierOn = false;
    return;
  }
  if (!humidifierOn && avgH < humSetpoint - HUM_HYST) humidifierOn = true;
  else if (humidifierOn && avgH >= humSetpoint + HUM_HYST) humidifierOn = false;
}

void driveOutputs() {
  digitalWrite(PIN_HEATER, heaterOn ? HIGH : LOW);
  digitalWrite(PIN_PELTIER, peltierOn ? HIGH : LOW);
  digitalWrite(PIN_HUMIDIFIER, humidifierOn ? HIGH : LOW);
  digitalWrite(PIN_FAN, HIGH);   // blower runs continuously
}

void connectWifi(unsigned long timeoutMs) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long start = millis();
  Serial.print("WiFi");
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    delay(400);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) Serial.println(WiFi.localIP());
}

void postReading(float temps[4], float hums[4]) {
  HTTPClient http;
  String url = String("http://") + DASHBOARD_HOST + ":" + DASHBOARD_PORT + "/api/reading";
  http.begin(url);
  http.addHeader("Content-Type", "application/json");

  String body = "{";
  time_t now = time(nullptr);
  if (now > 1700000000) {        // NTP synced: send the real sample time (epoch ms)
    char tbuf[24];
    snprintf(tbuf, sizeof(tbuf), "%llu", (unsigned long long)now * 1000ULL);
    body += String("\"time\":") + tbuf + ",";
  }
  body += "\"temps\":[" + numOrNull(temps[0]) + "," + numOrNull(temps[1]) + "," +
          numOrNull(temps[2]) + "," + numOrNull(temps[3]) + "],";
  body += "\"hums\":[" + numOrNull(hums[0]) + "," + numOrNull(hums[1]) + "," +
          numOrNull(hums[2]) + "," + numOrNull(hums[3]) + "],";
  body += "\"setpoints\":{\"temp\":" + String(tempSetpoint, 1) +
          ",\"hum\":" + String(humSetpoint, 1) + "},";
  body += "\"actuators\":{\"heater\":" + String(heaterOn ? "true" : "false") +
          ",\"peltier\":" + String(peltierOn ? "true" : "false") +
          ",\"fan\":true,\"humidifier\":" + String(humidifierOn ? "true" : "false") + "}";
  body += "}";

  int code = http.POST(body);
  Serial.printf("POST %d %s\n", code, body.c_str());
  if (code == 200) applyServerSetpoints(http.getString());
  http.end();
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_HEATER, OUTPUT);
  pinMode(PIN_PELTIER, OUTPUT);
  pinMode(PIN_FAN, OUTPUT);
  pinMode(PIN_HUMIDIFIER, OUTPUT);
  digitalWrite(PIN_HEATER, LOW);
  digitalWrite(PIN_PELTIER, LOW);
  digitalWrite(PIN_HUMIDIFIER, LOW);
  digitalWrite(PIN_FAN, HIGH);

  for (int i = 0; i < 4; i++) sensors[i]->begin();

  connectWifi(20000);                      // control keeps running even if Wi-Fi fails
  configTime(0, 0, "pool.ntp.org", "time.google.com");
}

void loop() {
  if (WiFi.status() != WL_CONNECTED && millis() - lastWifiTry > WIFI_RETRY_MS) {
    lastWifiTry = millis();
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASS);
  }

  if (millis() - lastSample < SAMPLE_INTERVAL_MS) return;
  lastSample = millis();

  float temps[4];
  float hums[4];
  float tSum = 0, hSum = 0;
  int tCount = 0, hCount = 0;

  for (int i = 0; i < 4; i++) {
    temps[i] = sensors[i]->readTemperature();
    hums[i] = sensors[i]->readHumidity();
    if (!isnan(temps[i])) { tSum += temps[i]; tCount++; }
    if (!isnan(hums[i])) { hSum += hums[i]; hCount++; }
  }

  float avgT = tCount ? tSum / tCount : NAN;
  float avgH = hCount ? hSum / hCount : NAN;

  controlTemperature(avgT);
  controlHumidity(avgH);
  driveOutputs();

  if (WiFi.status() == WL_CONNECTED && tCount > 0) {
    postReading(temps, hums);
  }
}
