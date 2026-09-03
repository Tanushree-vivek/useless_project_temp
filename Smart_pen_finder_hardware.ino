#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>

#define LED_PIN 25
#define BUZZER_PIN 26

// -------------------------------------------------------
// PHONE HOTSPOT CREDENTIALS  (fill these in)
// -------------------------------------------------------
const char* ssid     = "Tanu";
const char* password = "Lavith$123";

// -------------------------------------------------------
// mDNS HOSTNAME
// -------------------------------------------------------
// Once this is running, you can open the webpage using:
//   http://penfinder.local/status
//   http://penfinder.local/find
// instead of typing a numeric IP every time. (Some Android
// hotspots don't forward mDNS/multicast traffic well — if
// penfinder.local doesn't resolve, fall back to the IP
// printed on Serial.)
const char* mdnsHostname = "penfinder";

// -------------------------------------------------------
// RSSI -> DISTANCE CALIBRATION
// -------------------------------------------------------
// measuredPower = RSSI (dBm) measured at exactly 1 meter from the phone.
// You MUST calibrate this for your hardware/environment:
//   1. Flash this code, open Serial Monitor.
//   2. Hold the ESP32 exactly 1 meter from the phone.
//   3. Note the "RSSI" value it prints, put it here.
const float measuredPower = -50.0;   // dBm at 1m (typical range -40 to -60)
const float pathLossExponent = 2.5;  // 2 = free space, 2.5-4 = indoors/obstacles

const float DISTANCE_THRESHOLD_M = 1;

// Simple moving average to smooth noisy RSSI readings
const int RSSI_SAMPLES = 8;
int rssiBuffer[RSSI_SAMPLES];
int rssiIndex = 0;
bool bufferFilled = false;

WebServer server(80);

// FIND command state (non-blocking, same pattern as before)
bool findActive = false;
unsigned long findStartMillis = 0;
const unsigned long FIND_DURATION_MS = 3000;

bool isFar = false; // current alert state

unsigned long lastRssiCheck = 0;
const unsigned long RSSI_CHECK_INTERVAL_MS = 500;

unsigned long lastReconnectAttempt = 0;
const unsigned long RECONNECT_INTERVAL_MS = 5000;

// -------------------------------------------------------
// Convert RSSI (dBm) to estimated distance (meters)
// -------------------------------------------------------
float rssiToDistance(float rssi) {
  return pow(10.0, (measuredPower - rssi) / (10.0 * pathLossExponent));
}

float getSmoothedRssi(int newSample) {
  rssiBuffer[rssiIndex] = newSample;
  rssiIndex = (rssiIndex + 1) % RSSI_SAMPLES;
  if (rssiIndex == 0) bufferFilled = true;

  int count = bufferFilled ? RSSI_SAMPLES : rssiIndex;
  long sum = 0;
  for (int i = 0; i < count; i++) sum += rssiBuffer[i];
  return (float)sum / count;
}

// -------------------------------------------------------
// CORS helper — call at the top of every route handler so
// the webpage (running on your phone/laptop browser) is
// allowed to fetch() this ESP32 across the local network.
// -------------------------------------------------------
void addCorsHeaders() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "*");
}

// -------------------------------------------------------
// HTTP handlers (replaces old BLE FIND command)
// -------------------------------------------------------
void handleFind() {
  addCorsHeaders();
  Serial.println(">>> FIND COMMAND (HTTP) <<<");
  findActive = true;
  findStartMillis = millis();
  digitalWrite(LED_PIN, HIGH);
  digitalWrite(BUZZER_PIN, HIGH);
  server.send(200, "text/plain", "FIND triggered");
}

void handleStatus() {
  addCorsHeaders();
  String json = "{\"far\":" + String(isFar ? "true" : "false") +
                ",\"rssi\":" + String(WiFi.RSSI()) + "}";
  server.send(200, "application/json", json);
}

void handleRoot() {
  addCorsHeaders();
  server.send(200, "text/plain", "Smart Pen Finder (WiFi) is running");
}

// Handles CORS "preflight" OPTIONS requests that browsers send
// automatically before the real GET request. Without this, some
// browsers silently block every fetch() call from the webpage.
void handleNotFoundOrOptions() {
  if (server.method() == HTTP_OPTIONS) {
    addCorsHeaders();
    server.send(204);
  } else {
    server.send(404, "text/plain", "Not found");
  }
}

void connectToHotspot() {
  Serial.print("Connecting to hotspot: ");
  Serial.println(ssid);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(300); // only during initial connect, not in loop()
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.print("Connected! IP address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println();
    Serial.println("Failed to connect (will keep retrying in background).");
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(LED_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  digitalWrite(BUZZER_PIN, LOW);

  Serial.println();
  Serial.println("==============================");
  Serial.println("   SMART PEN FINDER (WiFi)");
  Serial.println("==============================");

  connectToHotspot();

  // Start mDNS so the device answers to http://penfinder.local
  // in addition to its numeric IP. Only works if WiFi connected.
  if (WiFi.status() == WL_CONNECTED) {
    if (MDNS.begin(mdnsHostname)) {
      Serial.print("mDNS started: http://");
      Serial.print(mdnsHostname);
      Serial.println(".local");
    } else {
      Serial.println("mDNS failed to start (IP address will still work).");
    }
  }

  server.on("/", handleRoot);
  server.on("/find", handleFind);
  server.on("/status", handleStatus);
  server.onNotFound(handleNotFoundOrOptions);

  server.begin();
  Serial.println("HTTP server started (routes: /find, /status)");
  Serial.println("==============================");
}

void loop() {
  server.handleClient();

  unsigned long currentMillis = millis();

  // -------------------------
  // Keep WiFi alive (non-blocking reconnect)
  // -------------------------
  if (WiFi.status() != WL_CONNECTED) {
    if (currentMillis - lastReconnectAttempt >= RECONNECT_INTERVAL_MS) {
      lastReconnectAttempt = currentMillis;
      Serial.println("WiFi disconnected, retrying...");
      WiFi.disconnect();
      WiFi.begin(ssid, password);
    }
    return; // skip distance logic while disconnected
  }

  // -------------------------
  // Handle FIND timeout (non-blocking, same as original)
  // -------------------------
  if (findActive) {
    if (currentMillis - findStartMillis >= FIND_DURATION_MS) {
      findActive = false;
      digitalWrite(BUZZER_PIN, isFar ? HIGH : LOW);
      digitalWrite(LED_PIN, isFar ? HIGH : LOW);
      Serial.println("Find completed");
    }
    return; // skip normal distance logic while FIND is active
  }

  // -------------------------
  // Distance check via RSSI, every RSSI_CHECK_INTERVAL_MS
  // -------------------------
  if (currentMillis - lastRssiCheck >= RSSI_CHECK_INTERVAL_MS) {
    lastRssiCheck = currentMillis;

    int rawRssi = WiFi.RSSI();
    float smoothRssi = getSmoothedRssi(rawRssi);
    float distance = rssiToDistance(smoothRssi);

    isFar = (distance > DISTANCE_THRESHOLD_M);

    digitalWrite(LED_PIN, isFar ? HIGH : LOW);
    digitalWrite(BUZZER_PIN, isFar ? HIGH : LOW);

    Serial.print("RSSI: ");
    Serial.print(rawRssi);
    Serial.print(" dBm | smoothed: ");
    Serial.print(smoothRssi, 1);
    Serial.print(" dBm | est. distance: ");
    Serial.print(distance, 2);
    Serial.print(" m | state: ");
    Serial.println(isFar ? "FAR (alert ON)" : "NEAR (alert OFF)");
  }
}
