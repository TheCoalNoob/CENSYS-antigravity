#include <SPI.h>
#include <LoRa.h>
#include <DHT.h>
#include <TinyGPSPlus.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

// =====================================================
// NODE SETTINGS (CHANGE PER NODE)
// =====================================================
#define NODE_ID 4
String NODE_PASSKEY = "CENSYS_N4_2026";   // change per node

// =====================================================
// BARANGAY BOUNDARY POLYGONS (for GPS auto-detection)
// Coordinates traced from Google Maps
// =====================================================
const int KALUNASAN_PTS = 16;
const float KALUNASAN_POLY[][2] = {
  {10.3415, 123.8759}, {10.3410, 123.8805}, {10.3395, 123.8842},
  {10.3380, 123.8870}, {10.3350, 123.8895}, {10.3318, 123.8905},
  {10.3290, 123.8895}, {10.3258, 123.8859}, {10.3245, 123.8841},
  {10.3245, 123.8800}, {10.3260, 123.8775}, {10.3290, 123.8760},
  {10.3310, 123.8758}, {10.3340, 123.8755}, {10.3370, 123.8752},
  {10.3392, 123.8755}
};
const int SANNICOLAS_PTS = 12;
const float SANNICOLAS_POLY[][2] = {
  {10.2962, 123.8896}, {10.2960, 123.8918}, {10.2952, 123.8930},
  {10.2940, 123.8928}, {10.2927, 123.8924}, {10.2917, 123.8910},
  {10.2920, 123.8897}, {10.2927, 123.8885}, {10.2934, 123.8872},
  {10.2945, 123.8864}, {10.2955, 123.8870}, {10.2960, 123.8880}
};
const int KALUBIHAN_PTS = 10;
const float KALUBIHAN_POLY[][2] = {
  {10.2990, 123.8963}, {10.2989, 123.8980}, {10.2985, 123.8998},
  {10.2978, 123.9004}, {10.2965, 123.9003}, {10.2952, 123.8993},
  {10.2951, 123.8978}, {10.2958, 123.8963}, {10.2970, 123.8955},
  {10.2980, 123.8957}
};

// Barangay centers for fallback nearest-match
const float BRGY_CENTERS[][2] = {
  {10.3290849, 123.8869029},  // kalunasan
  {10.295138, 123.8907164},   // sannicolas
  {10.2991276, 123.8956305}   // kalubihan
};
const char* BRGY_NAMES[] = {"kalunasan", "sannicolas", "kalubihan"};

bool pointInPoly(float lat, float lng, const float poly[][2], int nPts) {
  bool inside = false;
  for (int i = 0, j = nPts - 1; i < nPts; j = i++) {
    float yi = poly[i][0], xi = poly[i][1];
    float yj = poly[j][0], xj = poly[j][1];
    if (((yi > lat) != (yj > lat)) && (lng < (xj - xi) * (lat - yi) / (yj - yi) + xi))
      inside = !inside;
  }
  return inside;
}

float gpsDist(float lat1, float lng1, float lat2, float lng2) {
  float dLat = (lat2 - lat1) * 0.0174533;
  float dLng = (lng2 - lng1) * 0.0174533;
  float a = sin(dLat/2)*sin(dLat/2) + cos(lat1*0.0174533)*cos(lat2*0.0174533)*sin(dLng/2)*sin(dLng/2);
  return 6371000.0 * 2.0 * atan2(sqrt(a), sqrt(1-a));
}

String getBarangayFromCoords(float lat, float lng) {
  if (lat == 0.0 || lng == 0.0) return "";
  if (pointInPoly(lat, lng, KALUNASAN_POLY, KALUNASAN_PTS)) return "kalunasan";
  if (pointInPoly(lat, lng, SANNICOLAS_POLY, SANNICOLAS_PTS)) return "sannicolas";
  if (pointInPoly(lat, lng, KALUBIHAN_POLY, KALUBIHAN_PTS)) return "kalubihan";
  // Fallback: nearest center within 800m
  float minDist = 999999;
  int nearest = -1;
  for (int i = 0; i < 3; i++) {
    float d = gpsDist(lat, lng, BRGY_CENTERS[i][0], BRGY_CENTERS[i][1]);
    if (d < minDist) { minDist = d; nearest = i; }
  }
  if (nearest >= 0 && minDist < 800) return String(BRGY_NAMES[nearest]);
  return "";
}

// Cached barangay — CLEARED on boot to force GPS re-detection
// This prevents updating the wrong barangay after relocation
String cachedBarangay = "";

// =====================================================
// FIRE LOCK — once Fire is confirmed, LOCK the category
// at Fire until operator confirms fire is out via website
// =====================================================
bool fireLocked = false;
unsigned long fireLockedTime = 0;
unsigned long lastFireClearCheck = 0;
const unsigned long FIRE_CLEAR_CHECK_INTERVAL_MS = 15000;  // Check every 15s

// =====================================================
// WIFI CREDENTIALS
// =====================================================
const char* STA_SSID = "IKYK";
const char* STA_PASS = "444everydayOK";

// =====================================================
// FIREBASE CONFIG (same as gateway)
// =====================================================
#define FIREBASE_HOST "https://censys-antigravity-default-rtdb.asia-southeast1.firebasedatabase.app"
#define FIREBASE_AUTH "gXuLrZRM6GoMQKKwuHzWNDyp2Sd3x4CefsNG5kFc"

// =====================================================
// LORA PINS
// =====================================================
#define LORA_SS   5
#define LORA_RST  14
#define LORA_DIO0 26

// =====================================================
// SENSOR PINS
// =====================================================
#define DHTPIN     4
#define DHTTYPE    DHT22
#define FLAME_PIN  33
#define MQ2_PIN    34

// =====================================================
// STATUS LED
// =====================================================
#define LED_PIN    25

// =====================================================
// GPS UART
// =====================================================
HardwareSerial gpsSerial(2);
TinyGPSPlus gps;
DHT dht(DHTPIN, DHTTYPE);

// =====================================================
// GPS CACHED COORDINATES
// =====================================================
double cachedLat = 0.0;
double cachedLng = 0.0;
bool hasCachedGPS = false;
unsigned long lastGPSFixMillis = 0;
const unsigned long GPS_CACHE_MAX_AGE_MS = 86400000;  // 24 hours — nodes are stationary, cache aggressively

// =====================================================
// WIFI STATE
// =====================================================
bool wifiConnected = false;
const unsigned long WIFI_CONNECT_TIMEOUT_MS = 8000;
const unsigned long WIFI_RETRY_INTERVAL_MS  = 60000;
unsigned long lastWifiRetry = 0;
const unsigned long WIFI_SEND_INTERVAL_MS   = 10000;

// =====================================================
// TDMA TIMING (LoRa mode)
// =====================================================
const unsigned long TDMA_CYCLE_MS   = 12000;
const unsigned long TDMA_SLOT_MS    = 3000;
const unsigned long ACK_TIMEOUT_MS  = 1500;
const int MAX_HOPS = 3;

// =====================================================
// GATEWAY / LORA DISCOVERY
// =====================================================
const unsigned long BEACON_TIMEOUT_MS  = 45000;
const unsigned long STARTUP_LISTEN_MS  = 6000;
const unsigned long GATEWAY_FALLBACK_MS = 30000;  // 30s before falling back to relay chain (was 15s — more sticky)
unsigned long lastGatewayContact       = 0;
bool gatewayReachable                  = false;
bool gatewayLocked                     = false;   // Once ACK received, lock onto gateway
unsigned long gatewayLostTime          = 0;        // When gateway failures started
const int RELAY_MODE_ACK_THRESHOLD = 8;            // More tolerant of collision-caused ACK misses

// =====================================================
// STATE
// =====================================================
unsigned long lastSendTime = 0;
unsigned long lastAckTime = 0;
unsigned long lastSuccessfulSendTime = 0;  // Tracks last ACTUAL successful data delivery
unsigned long lastLedBlink = 0;
bool ledState = false;
unsigned long seqCounter = 0;
int noAckCount = 0;
bool loraInitOk = false;

// =====================================================
// DUPLICATE TRACKER
// =====================================================
const int SEEN_MAX = 30;
String seenPackets[SEEN_MAX];
int seenIndex = 0;

// =====================================================
// GPS INITIALIZATION — Maximized for indoor performance
// =====================================================
void initGPSModule() {
  // 1. Update rate: 1 Hz (1000ms) — standard, reliable
  gpsSerial.println("$PMTK220,1000*1F");   delay(100);

  // 2. Enable ALL sentence types for maximum satellite data
  //    GLL, RMC, VTG, GGA, GSA, GSV — more data = faster fix
  gpsSerial.println("$PMTK314,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0*28"); delay(100);

  // 3. Enable SBAS (satellite-based augmentation) — improves accuracy
  gpsSerial.println("$PMTK313,1*2E");       delay(100);

  // 4. DGPS mode: SBAS — use correction data for better positioning
  gpsSerial.println("$PMTK301,2*2E");       delay(100);

  // 5. Enable AIC (Active Interference Cancellation) — filters noise
  gpsSerial.println("$PMTK286,1*23");       delay(100);

  // 6. Enable EASY (Embedded Assist System) — predicts satellite
  //    orbits for faster cold/warm starts, critical for indoor
  gpsSerial.println("$PMTK869,1,1*35");     delay(100);

  // 7. Fitness/pedestrian navigation mode — optimized for
  //    stationary/slow-moving use, better weak-signal tracking
  gpsSerial.println("$PMTK886,1*25");       delay(100);

  // 8. Full power mode — no power saving, maximum satellite search
  gpsSerial.println("$PMTK225,0*2B");       delay(100);

  // 9. Search ALL satellite systems (GPS + GLONASS if supported)
  gpsSerial.println("$PMTK353,1,1,0,0,0*2A"); delay(100);

  Serial.println("GPS: indoor-optimized init commands sent");
}

// =====================================================
// HELPERS
// =====================================================
String getField(String data, int index) {
  int found = 0, start = 0, end = -1;
  for (int i = 0; i < (int)data.length(); i++) {
    if (data.charAt(i) == '|') {
      found++;
      start = end + 1;
      end = i;
      if (found - 1 == index) return data.substring(start, end);
    }
  }
  if (found == index) return data.substring(end + 1);
  return "";
}

bool isSeen(String sig) {
  for (int i = 0; i < SEEN_MAX; i++) { if (seenPackets[i] == sig) return true; }
  return false;
}

void addSeen(String sig) {
  seenPackets[seenIndex] = sig;
  seenIndex = (seenIndex + 1) % SEEN_MAX;
}

String fireStatus(int flameDigital, float temp, int smoke) {
  if (flameDigital != LOW && flameDigital != HIGH) return "Needs replacement";
  if (flameDigital == LOW) {
    // Flame sensor triggered — require corroboration to prevent sunlight false positives
    // Need temp >= 45°C OR smoke >= 450 to confirm actual fire
    if (temp >= 45.0 || smoke >= 450) {
      return "Flame";  // Corroborated — real fire
    } else {
      return "Warning-Flame";  // Uncorroborated — likely sunlight/IR noise
    }
  }
  return "None";
}

bool isDHTBad(float temp, float hum) {
  if (isnan(temp) || isnan(hum)) return true;
  return (temp < -20 || temp > 80 || hum < 0 || hum > 100);
}

bool isMQ2Bad(int mq2) { return (mq2 < 0 || mq2 > 4095); }
bool isFlameBad(int flameVal) { return (flameVal != LOW && flameVal != HIGH); }

String sensorHealthText(float temp, float hum, int mq2, int flameVal, bool gpsLive, bool gpsCached) {
  String issues = "";
  if (isDHTBad(temp, hum)) issues += "DHT22 Needs replacement;";
  if (isMQ2Bad(mq2)) issues += "MQ2 Needs replacement;";
  if (isFlameBad(flameVal)) issues += "FlameSensor Needs replacement;";
  if (!gpsLive && !gpsCached) issues += "GPS No Fix;";
  else if (!gpsLive && gpsCached) issues += "GPS Cached;";
  return (issues.length() == 0) ? "OK" : issues;
}

bool needsMaintenance(float temp, float hum, int mq2, int flameVal) {
  return isDHTBad(temp, hum) || isMQ2Bad(mq2) || isFlameBad(flameVal);
}

String esc(String s) {
  s.replace("\\", "\\\\"); s.replace("\"", "\\\"");
  s.replace("\n", " "); s.replace("\r", " ");
  return s;
}

bool isReplacementValue(String v) {
  v.trim();
  return (v == "Needs replacement" || v.length() == 0);
}

bool isNodeInPath(String path, int nodeId) {
  String nodeStr = String(nodeId);
  int pathLen = path.length(), nodeStrLen = nodeStr.length();
  for (int i = 0; i <= pathLen - nodeStrLen; i++) {
    bool match = true;
    for (int j = 0; j < nodeStrLen; j++) {
      if (path.charAt(i + j) != nodeStr.charAt(j)) { match = false; break; }
    }
    if (match) {
      bool leftOk = (i == 0) || (path.charAt(i - 1) == '>');
      bool rightOk = (i + nodeStrLen >= pathLen) || (path.charAt(i + nodeStrLen) == '>');
      if (leftOk && rightOk) return true;
    }
  }
  return false;
}

// =====================================================
// CATEGORY CLASSIFICATION (same as gateway)
// Sensor-count based: count HIGH sensors, 3+=Fire, 2=Warning, 0-1=Normal
// =====================================================
String classifyCategory(String tempStr, String humStr, String smokeStr, String fireStr, String healthStr) {
  if (healthStr.indexOf("Needs replacement") >= 0) return "Warning";
  float temp = tempStr.toFloat();
  float hum  = humStr.toFloat();
  int smoke  = smokeStr.toInt();
  bool flame = (fireStr == "Flame" || fireStr == "Warning-Flame");

  int highCount = 0;

  // Sensor 1: Temperature >= 42°C
  if (!isReplacementValue(tempStr) && temp >= 42.0) highCount++;

  // Sensor 2: Humidity <= 25% (inverted — LOW humidity = HIGH danger)
  if (!isReplacementValue(humStr) && hum <= 25.0 && hum > 0.0) highCount++;

  // Sensor 3: Smoke >= 400
  if (!isReplacementValue(smokeStr) && smoke >= 400) highCount++;

  // Sensor 4: Fire/Flame sensor
  if (!isReplacementValue(fireStr) && flame) highCount++;

  if (highCount >= 3) return "Fire";
  if (highCount == 2) return "Warning";
  return "Normal";
}

// =====================================================
// FIRE LOCK CATEGORY WRAPPER
// Once Fire is detected, lock it until operator clears
// =====================================================
String getLockedCategory(String tempStr, String humStr, String mq2Str, String fireStr, String healthStr) {
  String raw = classifyCategory(tempStr, humStr, mq2Str, fireStr, healthStr);
  
  // If we hit Fire for the first time, lock it
  if (raw == "Fire" && !fireLocked) {
    fireLocked = true;
    fireLockedTime = millis();
    Serial.println("FIRE LOCKED: Category locked at Fire until operator confirms fire is out");
  }
  
  // If locked, always return Fire
  if (fireLocked) return "Fire";
  
  return raw;
}

// =====================================================
// CHECK FIRE_CLEARED (WiFi mode only)
// Reads fire_cleared flag from Firebase to unlock
// =====================================================
void checkFireClearedWiFi() {
  if (!wifiConnected || WiFi.status() != WL_CONNECTED) return;
  if (!fireLocked) return;
  if (millis() - lastFireClearCheck < FIRE_CLEAR_CHECK_INTERVAL_MS) return;
  lastFireClearCheck = millis();
  
  String brgy = cachedBarangay;
  if (brgy.length() == 0) return;
  
  HTTPClient http;
  WiFiClientSecure client;
  client.setInsecure();
  
  String url = String(FIREBASE_HOST) + "/fire_status/" + brgy + "/fire_cleared.json?auth=" + String(FIREBASE_AUTH);
  http.begin(client, url);
  int httpCode = http.GET();
  
  if (httpCode == 200) {
    String payload = http.getString();
    payload.trim();
    if (payload == "true") {
      fireLocked = false;
      fireLockedTime = 0;
      Serial.println("FIRE CLEARED: Unlocked by operator via website");
    }
  }
  http.end();
}

// =====================================================
// LED STATUS
//
// SOLID         = Connected & healthy (WiFi or LoRa)
// MEDIUM BLINK  = Searching for LoRa to transmit/receive
// FAST BLINK    = Sensor needs checkup/replacement
// =====================================================
void updateLED() {
  // Read sensors for maintenance check
  float temp = dht.readTemperature();
  float hum  = dht.readHumidity();
  int mq2    = analogRead(MQ2_PIN);
  int flame  = digitalRead(FLAME_PIN);
  bool maintenance = needsMaintenance(temp, hum, mq2, flame);

  // Network is OK if WiFi is connected, OR if we successfully sent data within 60s
  bool networkOk = wifiConnected || ((millis() - lastSuccessfulSendTime) < 60000);

  // 1. SOLID = Connected & healthy (WiFi or LoRa)
  if (networkOk && !maintenance) {
    digitalWrite(LED_PIN, HIGH);
    ledState = true;
  }
  // 2. MEDIUM BLINK = Searching for LoRa (only after 60s of no successful send)
  else if (!networkOk) {
    if (millis() - lastLedBlink >= 400) {
      lastLedBlink = millis();
      ledState = !ledState;
      digitalWrite(LED_PIN, ledState);
    }
  }
  // 3. FAST BLINK = Sensor needs checkup/replacement
  else if (maintenance) {
    if (millis() - lastLedBlink >= 150) {
      lastLedBlink = millis();
      ledState = !ledState;
      digitalWrite(LED_PIN, ledState);
    }
  }
}

// =====================================================
// FEED GPS
// =====================================================
void feedGPS() {
  while (gpsSerial.available()) gps.encode(gpsSerial.read());
}

// =====================================================
// LORA SEND — blocking
// =====================================================
void sendLoRa(String packet) {
  if (!loraInitOk) return;
  LoRa.idle();
  LoRa.beginPacket();
  LoRa.print(packet);
  LoRa.endPacket();
  delay(20);
  LoRa.receive();
}

// =====================================================
// TDMA SLOT CHECK
// =====================================================
bool isMySlot() {
  unsigned long cycleTime = millis() % TDMA_CYCLE_MS;
  unsigned long mySlotStart = (unsigned long)(NODE_ID - 1) * TDMA_SLOT_MS;
  return (cycleTime >= mySlotStart && cycleTime < mySlotStart + TDMA_SLOT_MS);
}

// =====================================================
// WAIT FOR ACK
// =====================================================
bool waitForAck(unsigned long mySeq) {
  unsigned long t0 = millis();
  while (millis() - t0 < ACK_TIMEOUT_MS) {
    feedGPS();
    int packetSize = LoRa.parsePacket();
    if (!packetSize) { delay(5); continue; }
    String rx = "";
    while (LoRa.available()) rx += (char)LoRa.read();
    rx.trim();

    String key  = getField(rx, 0);
    String type = getField(rx, 1);

    if (type == "BEACON" && key == "CENSYS_GW") {
      lastGatewayContact = millis(); gatewayReachable = true;
      gatewayLocked = true; gatewayLostTime = 0;
      continue;
    }
    if (type == "ACK" && key == NODE_PASSKEY) {
      String dest = getField(rx, 2);
      String seq  = getField(rx, 3);
      if (dest.toInt() == NODE_ID && seq.toInt() == (int)mySeq) {
        lastAckTime = millis(); lastGatewayContact = millis();
        lastSuccessfulSendTime = millis();
        gatewayReachable = true; gatewayLocked = true;
        gatewayLostTime = 0; noAckCount = 0;
        return true;
      }
    }
  }
  return false;
}

// =====================================================
// PUSH ANOTHER NODE'S DATA TO FIREBASE (WiFi relay)
// When this node has WiFi and receives LoRa data from
// a node that doesn't have WiFi, push it to Firebase
// on behalf of that node — so the website sees it.
// =====================================================
void pushOtherNodeToFirebase(int originNode, String passkey,
                              String tempStr, String humStr, String mq2Str,
                              String fireStr, String latStr, String lngStr,
                              String healthStr, String pathStr) {
  if (WiFi.status() != WL_CONNECTED) { wifiConnected = false; return; }

  // Don't relay blank/empty data — node is probably off
  if (tempStr.length() == 0 && humStr.length() == 0 && mq2Str.length() == 0 && fireStr.length() == 0) {
    Serial.println("WiFi RELAY N" + String(originNode) + " SKIPPED — all sensor data blank");
    return;
  }

  String category = classifyCategory(tempStr, humStr, mq2Str, fireStr, healthStr);

  // Determine barangay from ORIGIN NODE's GPS (not relay node's)
  String brgy = "";
  float originLat = latStr.toFloat();
  float originLng = lngStr.toFloat();
  if (originLat != 0.0 && originLng != 0.0) {
    brgy = getBarangayFromCoords(originLat, originLng);
  }
  // Fallback: use this relay node's barangay if origin has no GPS
  if (brgy.length() == 0 && cachedBarangay.length() > 0) {
    brgy = cachedBarangay;
  }
  if (brgy.length() == 0) brgy = "unregistered";

  HTTPClient http;
  WiFiClientSecure client;
  client.setInsecure();

  String basePath = (brgy == "unregistered") ? "/unregistered_nodes/node" : "/barangays/" + brgy + "/node";
  String url = String(FIREBASE_HOST) + basePath + String(originNode) + ".json?auth=" + String(FIREBASE_AUTH);

  String json = "{";
  json += "\"online\":true,";
  json += "\"node\":" + String(originNode) + ",";
  json += "\"barangay\":\"" + brgy + "\",";
  json += "\"last_sender\":" + String(NODE_ID) + ",";
  json += "\"seq\":0,";
  json += "\"hops\":0,";
  json += "\"temp\":\"" + esc(tempStr) + "\",";
  json += "\"humid\":\"" + esc(humStr) + "\",";
  json += "\"smoke\":\"" + esc(mq2Str) + "\",";
  json += "\"fire\":\"" + esc(fireStr) + "\",";
  json += "\"lat\":\"" + esc(latStr) + "\",";
  json += "\"lng\":\"" + esc(lngStr) + "\",";
  json += "\"health\":\"" + esc(healthStr) + "\",";
  json += "\"path\":\"" + esc(pathStr) + ">" + String(NODE_ID) + "(wifi)\",";
  json += "\"category\":\"" + esc(category) + "\",";
  json += "\"rssi\":0,";
  json += "\"last_seen_sec\":0,";
  json += "\"passkey\":\"" + esc(passkey) + "\",";
  json += "\"timestamp\":{\".sv\":\"timestamp\"}";
  json += "}";

  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");
  int httpCode = http.PUT(json);

  if (httpCode > 0) {
    Serial.println("WiFi RELAY N" + String(originNode) + " → Firebase: HTTP " + String(httpCode));
  } else {
    Serial.println("WiFi RELAY N" + String(originNode) + " Firebase error");
  }
  http.end();
}

// =====================================================
// PROCESS INCOMING — LoRa listener
//
// WiFi mode:  LISTEN only. If we receive another node's
//             data via LoRa, push it to Firebase for them.
//             We NEVER transmit on LoRa in WiFi mode.
//
// LoRa mode:  Listen + relay via LoRa to gateway.
// =====================================================
void processIncoming() {
  if (!loraInitOk) return;
  feedGPS();
  int packetSize = LoRa.parsePacket();
  if (!packetSize) return;

  String rx = "";
  while (LoRa.available()) rx += (char)LoRa.read();
  rx.trim();
  if (rx.length() < 5) return;

  String key  = getField(rx, 0);
  String type = getField(rx, 1);

  // Gateway beacon
  if (type == "BEACON" && key == "CENSYS_GW") {
    lastGatewayContact = millis(); gatewayReachable = true;
    gatewayLocked = true; gatewayLostTime = 0;
    Serial.println("BEACON from gateway (LOCKED)");
    return;
  }

  // ACK for us
  if (type == "ACK") {
    String dest = getField(rx, 2);
    if (dest.toInt() == NODE_ID) {
      lastAckTime = millis(); lastGatewayContact = millis();
      lastSuccessfulSendTime = millis();
      gatewayReachable = true; gatewayLocked = true;
      gatewayLostTime = 0; noAckCount = 0;
    }
    return;
  }

  if (type != "DATA") return;

  // Validate passkey — must be a valid CENSYS node
  if (!(key == "CENSYS_N1_2026" || key == "CENSYS_N2_2026" ||
        key == "CENSYS_N3_2026" || key == "CENSYS_N4_2026")) return;

  String originStr = getField(rx, 2);
  String senderStr = getField(rx, 3);
  String seqStr    = getField(rx, 4);
  String hopsStr   = getField(rx, 5);
  String pathStr   = getField(rx, 13);

  int origin = originStr.toInt();
  int sender = senderStr.toInt();
  int hops   = hopsStr.toInt();

  if (origin == NODE_ID) return;

  String sig = key + "|" + originStr + "|" + seqStr;

  String tempStr   = getField(rx, 6);
  String humStr    = getField(rx, 7);
  String mq2Str    = getField(rx, 8);
  String fireStr   = getField(rx, 9);
  String latStr    = getField(rx, 10);
  String lngStr    = getField(rx, 11);
  String healthStr = getField(rx, 12);

  // ============================================
  // WiFi mode: push the other node's data to
  // Firebase directly (no LoRa transmit needed)
  // ============================================
  if (wifiConnected) {
    if (isSeen(sig)) return;
    addSeen(sig);
    Serial.println("WiFi RELAY: received N" + originStr + " via LoRa → pushing to Firebase");
    pushOtherNodeToFirebase(origin, key, tempStr, humStr, mq2Str,
                             fireStr, latStr, lngStr, healthStr, pathStr);
    return;
  }

  // ============================================
  // LoRa mode: STRICT CHAIN relay
  // 4 → 3 → 2 → 1 → Gateway
  //
  // Each node only relays data from its DIRECT
  // upstream neighbor (sender == NODE_ID + 1).
  // This prevents duplicate relays and circular
  // routing — data flows strictly down the chain.
  // ============================================
  if (sender != NODE_ID + 1) {
    // Not from our direct chain neighbor — ignore
    // (don't add to seen, so the chain relay can still be processed later)
    return;
  }
  if (isSeen(sig)) return;
  addSeen(sig);
  if (hops >= MAX_HOPS) return;
  if (isNodeInPath(pathStr, NODE_ID)) return;

  String newPath = pathStr + ">" + String(NODE_ID);
  String relayPacket =
    key + "|DATA|" + originStr + "|" + String(NODE_ID) + "|" +
    seqStr + "|" + String(hops + 1) + "|" +
    tempStr + "|" + humStr + "|" + mq2Str + "|" + fireStr + "|" +
    latStr + "|" + lngStr + "|" + healthStr + "|" + newPath;

  // No delay — relay immediately through the chain to minimize latency
  sendLoRa(relayPacket);
  Serial.println("RELAYED N" + originStr + " (hops:" + String(hops + 1) + ") chain: " + newPath);
}

// =====================================================
// GPS CACHE
// =====================================================
void updateGPSCache() {
  feedGPS();
  if (gps.location.isValid() && gps.location.isUpdated()) {
    double newLat = gps.location.lat();
    double newLng = gps.location.lng();
    // Validate coordinates are within Philippines bounding box
    if (newLat > 4.0 && newLat < 22.0 && newLng > 116.0 && newLng < 128.0) {
      // Accept ANY valid fix — indoor signals are weak, so we
      // take what we can get. Better a rough location than none.
      // If we already have a good fix (HDOP < 5), only update with equal or better.
      // If we have no fix yet, accept anything up to HDOP 15.
      float currentHdop = gps.hdop.isValid() ? gps.hdop.hdop() : 99.0;
      if (!hasCachedGPS || currentHdop < 15.0) {
        cachedLat = newLat; cachedLng = newLng; hasCachedGPS = true; lastGPSFixMillis = millis();
        // Auto-detect barangay from GPS
        String detectedBrgy = getBarangayFromCoords(newLat, newLng);
        if (detectedBrgy.length() > 0) {
          cachedBarangay = detectedBrgy;
          Serial.println("GPS AUTO-DETECT: Barangay = " + cachedBarangay);
        }
      }
    }
  }
}

bool getGPSCoordinates(double &lat, double &lng, String &gpsStatus) {
  if (gps.location.isValid()) { lat = gps.location.lat(); lng = gps.location.lng(); gpsStatus = "Live"; return true; }
  if (hasCachedGPS && (millis() - lastGPSFixMillis) < GPS_CACHE_MAX_AGE_MS) { lat = cachedLat; lng = cachedLng; gpsStatus = "Cached"; return true; }
  lat = 0.0; lng = 0.0; gpsStatus = "No Fix"; return false;
}

// =====================================================
// WIFI CONNECTION
// =====================================================
bool tryWiFiConnect() {
  Serial.print("WiFi: Connecting to "); Serial.println(STA_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(STA_SSID, STA_PASS);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < WIFI_CONNECT_TIMEOUT_MS) {
    delay(200); feedGPS(); Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi: CONNECTED → " + WiFi.localIP().toString());
    wifiConnected = true;
    return true;
  }

  Serial.println("WiFi: FAILED → using LoRa mode");
  WiFi.disconnect(true); WiFi.mode(WIFI_OFF);
  wifiConnected = false;
  return false;
}

// =====================================================
// FIREBASE PUSH (WiFi mode — same format as gateway)
// =====================================================
void pushToFirebaseViaWiFi(String tempStr, String humStr, String mq2Str,
                            String fireStr, String latStr, String lngStr,
                            String healthStr, String category) {
  if (WiFi.status() != WL_CONNECTED) { wifiConnected = false; return; }

  // Don't push blank data — all sensors failed or not ready
  if (tempStr.length() == 0 && humStr.length() == 0 && mq2Str.length() == 0 && fireStr.length() == 0) {
    Serial.println("Firebase push SKIPPED — all sensor data blank");
    return;
  }

  // Determine barangay from GPS
  String brgy = cachedBarangay;
  if (brgy.length() == 0) brgy = "unregistered";

  String basePath = (brgy == "unregistered") ? "/unregistered_nodes/node" : "/barangays/" + brgy + "/node";

  HTTPClient http;
  WiFiClientSecure client;
  client.setInsecure();

  String url = String(FIREBASE_HOST) + basePath + String(NODE_ID) + ".json?auth=" + String(FIREBASE_AUTH);

  String json = "{";
  json += "\"online\":true,";
  json += "\"node\":" + String(NODE_ID) + ",";
  json += "\"barangay\":\"" + brgy + "\",";
  json += "\"last_sender\":" + String(NODE_ID) + ",";
  json += "\"seq\":" + String(seqCounter) + ",";
  json += "\"hops\":0,";
  json += "\"temp\":\"" + esc(tempStr) + "\",";
  json += "\"humid\":\"" + esc(humStr) + "\",";
  json += "\"smoke\":\"" + esc(mq2Str) + "\",";
  json += "\"fire\":\"" + esc(fireStr) + "\",";
  json += "\"lat\":\"" + esc(latStr) + "\",";
  json += "\"lng\":\"" + esc(lngStr) + "\",";
  json += "\"health\":\"" + esc(healthStr) + "\",";
  json += "\"path\":\"" + String(NODE_ID) + "\",";
  json += "\"category\":\"" + esc(category) + "\",";
  json += "\"rssi\":0,";
  json += "\"last_seen_sec\":0,";
  json += "\"passkey\":\"" + esc(NODE_PASSKEY) + "\",";
  json += "\"timestamp\":{\".sv\":\"timestamp\"}";
  json += "}";

  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");
  int httpCode = http.PUT(json);

  if (httpCode > 0) {
    Serial.println("Firebase push: HTTP " + String(httpCode));
    lastAckTime = millis();
    lastSuccessfulSendTime = millis();
  } else {
    Serial.println("Firebase error: " + http.errorToString(httpCode));
  }
  http.end();
}

// =====================================================
// SETUP
// =====================================================
void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(FLAME_PIN, INPUT);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  dht.begin();
  gpsSerial.begin(9600, SERIAL_8N1, 16, 17);
  delay(500);
  initGPSModule();

  Serial.println("========================================");
  Serial.println("  CENSYS Node " + String(NODE_ID));
  Serial.println("  Barangay: Auto-detect from GPS");
  Serial.println("  (starts as UNREGISTERED until GPS fix)");
  Serial.println("========================================");

  // ===== STEP 1: TRY WIFI FIRST =====
  Serial.println("STEP 1: Trying WiFi...");
  bool gotWifi = tryWiFiConnect();

  // ===== STEP 2: INIT LORA (always — for relay support) =====
  Serial.println("STEP 2: Initializing LoRa...");
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);
  if (!LoRa.begin(433E6)) {
    Serial.println("LoRa init FAILED");
    loraInitOk = false;
    if (!gotWifi) {
      Serial.println("FATAL: No WiFi and no LoRa!");
      while (1) { digitalWrite(LED_PIN, !digitalRead(LED_PIN)); delay(120); }
    }
  } else {
    LoRa.setSpreadingFactor(7);
    LoRa.setSignalBandwidth(125E3);
    LoRa.setCodingRate4(8);
    LoRa.setTxPower(20, PA_OUTPUT_PA_BOOST_PIN);
    LoRa.setPreambleLength(12);
    LoRa.setSyncWord(0x34);
    LoRa.enableCrc();
    LoRa.setGain(0);
    LoRa.receive();
    loraInitOk = true;
    Serial.println("LoRa: SF7, BW125k, CR4/8, TX20dBm, Preamble12, Sync0x34");
  }

  randomSeed(analogRead(35) ^ (NODE_ID * 12345));

  // ===== STEP 3: IF NO WIFI → FIND GATEWAY OR NEAREST NODE =====
  if (!wifiConnected && loraInitOk) {
    Serial.println("STEP 3: Scanning for gateway...");
    unsigned long scanStart = millis();
    while (millis() - scanStart < STARTUP_LISTEN_MS) {
      feedGPS();
      int ps = LoRa.parsePacket();
      if (ps) {
        String rx = "";
        while (LoRa.available()) rx += (char)LoRa.read();
        rx.trim();
        if (getField(rx, 1) == "BEACON" && getField(rx, 0) == "CENSYS_GW") {
          lastGatewayContact = millis(); lastAckTime = millis();
          gatewayReachable = true; gatewayLocked = true;
          gatewayLostTime = 0;
          Serial.println(">>> Gateway FOUND & LOCKED <<<");
          break;
        }
      }
      delay(10);
    }
    if (!gatewayReachable) {
      Serial.println("Gateway not found — will send to nearest reachable node");
    }
  }

  // Print mode
  Serial.println("----------------------------------------");
  if (wifiConnected) {
    Serial.println("MODE: WiFi → Firebase (LED: SOLID)");
  } else if (gatewayLocked) {
    Serial.println("MODE: LoRa → Gateway LOCKED (LED: SOLID)");
  } else {
    Serial.println("MODE: LoRa → Nearest Node (LED: MEDIUM BLINK)");
  }
  Serial.println("TDMA slot: " + String((NODE_ID-1)*3) + "s-" + String(NODE_ID*3) + "s");
  Serial.println("----------------------------------------");
}

// =====================================================
// LOOP
// =====================================================
void loop() {
  feedGPS();
  updateGPSCache();

  // ===== WIFI STATUS CHECK =====
  if (wifiConnected && WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi LOST → switching to LoRa");
    wifiConnected = false;
    WiFi.disconnect(true); WiFi.mode(WIFI_OFF);
    if (loraInitOk) LoRa.receive();
  }

  // ===== RETRY WIFI PERIODICALLY =====
  if (!wifiConnected && (millis() - lastWifiRetry > WIFI_RETRY_INTERVAL_MS)) {
    lastWifiRetry = millis();
    Serial.println("WiFi: Retrying...");
    if (tryWiFiConnect()) {
      Serial.println("WiFi: RECOVERED");
    }
  }

  // =============================================================
  //  WiFi Mode — push directly to Firebase
  // =============================================================
  if (wifiConnected) {
    if (loraInitOk) processIncoming();  // Still help relay

    // Check fire_cleared flag periodically
    checkFireClearedWiFi();

    if (millis() - lastSendTime >= WIFI_SEND_INTERVAL_MS) {
      lastSendTime = millis();
      seqCounter++;

      float temp = dht.readTemperature();
      float hum  = dht.readHumidity();
      int mq2Raw = analogRead(MQ2_PIN);
      int flameVal = digitalRead(FLAME_PIN);

      double lat = 0.0, lng = 0.0;
      String gpsStatus = "No Fix";
      getGPSCoordinates(lat, lng, gpsStatus);
      bool gpsLive = gps.location.isValid();
      bool gpsCached = (hasCachedGPS && !gpsLive);

      String tempStr = isDHTBad(temp, hum) ? "Needs replacement" : String(temp, 1);
      String humStr  = isDHTBad(temp, hum) ? "Needs replacement" : String(hum, 1);
      String mq2Str  = isMQ2Bad(mq2Raw) ? "Needs replacement" : String(mq2Raw);
      String fireStr = isFlameBad(flameVal) ? "Needs replacement" : fireStatus(flameVal, temp, mq2Raw);
      String healthStr = sensorHealthText(temp, hum, mq2Raw, flameVal, gpsLive, gpsCached);
      String category = getLockedCategory(tempStr, humStr, mq2Str, fireStr, healthStr);

      Serial.println("==== WiFi TX Seq:" + String(seqCounter) + " ====");
      Serial.println("T:" + tempStr + " H:" + humStr + " S:" + mq2Str + " F:" + fireStr + " Cat:" + category);

      pushToFirebaseViaWiFi(tempStr, humStr, mq2Str, fireStr,
                             String(lat, 6), String(lng, 6), healthStr, category);
    }

    // LED: SOLID (WiFi connected & healthy)
    static unsigned long lastLedW = 0;
    if (millis() - lastLedW >= 500) { lastLedW = millis(); updateLED(); }
    return;
  }

  // =============================================================
  //  LoRa Mode — TDMA time-slotted transmission
  //  Sends to gateway if reachable, or to nearest node via relay
  // =============================================================

  // Gateway reachability — LOCK-ON logic
  // Once locked, keep sending to gateway. Only fallback after 15s of continuous failure.
  if (gatewayLocked) {
    gatewayReachable = true;  // Always send to gateway while locked

    // Start failure countdown if ACKs are being missed
    if (noAckCount >= 3 && gatewayLostTime == 0) {
      gatewayLostTime = millis();
      Serial.println("GW: ACK failures detected — starting 15s fallback countdown");
    }

    // Reset countdown if we get contact back
    if (noAckCount == 0 && gatewayLostTime > 0) {
      gatewayLostTime = 0;
      Serial.println("GW: Contact restored — fallback cancelled");
    }

    // After 15s of continuous failure → unlock and fallback to relay chain
    if (gatewayLostTime > 0 && (millis() - gatewayLostTime >= GATEWAY_FALLBACK_MS)) {
      gatewayLocked = false;
      gatewayReachable = false;
      gatewayLostTime = 0;
      Serial.println(">>> GATEWAY UNLOCKED — FALLBACK TO RELAY CHAIN <<<");
    }
  } else {
    // Not locked — check for beacon/ACK recovery to re-lock
    if (millis() - lastGatewayContact < BEACON_TIMEOUT_MS && noAckCount == 0) {
      gatewayReachable = true;
      gatewayLocked = true;
      gatewayLostTime = 0;
      Serial.println(">>> Gateway RECOVERED & RE-LOCKED <<<");
    }
  }

  processIncoming();

  // Only send during TDMA slot
  if (!isMySlot()) {
    static unsigned long lastLedL = 0;
    if (millis() - lastLedL >= 500) { lastLedL = millis(); updateLED(); }
    return;
  }

  // One send per cycle
  unsigned long currentCycle = millis() / TDMA_CYCLE_MS;
  static unsigned long lastSentCycle = 0;
  if (currentCycle == lastSentCycle) return;
  lastSentCycle = currentCycle;

  seqCounter++;

  float temp = dht.readTemperature();
  float hum  = dht.readHumidity();
  int mq2Raw = analogRead(MQ2_PIN);
  int flameVal = digitalRead(FLAME_PIN);

  double lat = 0.0, lng = 0.0;
  String gpsStatus = "No Fix";
  getGPSCoordinates(lat, lng, gpsStatus);
  bool gpsLive = gps.location.isValid();
  bool gpsCached = (hasCachedGPS && !gpsLive);

  String tempStr = isDHTBad(temp, hum) ? "Needs replacement" : String(temp, 1);
  String humStr  = isDHTBad(temp, hum) ? "Needs replacement" : String(hum, 1);
  String mq2Str  = isMQ2Bad(mq2Raw) ? "Needs replacement" : String(mq2Raw);
  String fireStr = isFlameBad(flameVal) ? "Needs replacement" : fireStatus(flameVal, temp, mq2Raw);
  String healthStr = sensorHealthText(temp, hum, mq2Raw, flameVal, gpsLive, gpsCached);

  String payload =
    NODE_PASSKEY + "|DATA|" +
    String(NODE_ID) + "|" + String(NODE_ID) + "|" +
    String(seqCounter) + "|0|" +
    tempStr + "|" + humStr + "|" + mq2Str + "|" + fireStr + "|" +
    String(lat, 6) + "|" + String(lng, 6) + "|" +
    healthStr + "|" + String(NODE_ID);

  addSeen(NODE_PASSKEY + "|" + String(NODE_ID) + "|" + String(seqCounter));

  Serial.println("==== LoRa TX [SLOT " + String(NODE_ID) + "] Seq:" + String(seqCounter) + " ====");
  Serial.println("T:" + tempStr + " H:" + humStr + " S:" + mq2Str + " F:" + fireStr);
  Serial.println("GW:" + String(gatewayLocked ? "LOCKED-DIRECT" : (gatewayReachable ? "DIRECT" : "RELAY→NEAREST")));

  // Random 1-3s delay before LoRa TX to stagger transmissions
  unsigned long loraDelay = random(1000, 3001);
  Serial.println("LoRa delay: " + String(loraDelay) + "ms");
  delay(loraDelay);

  sendLoRa(payload);
  bool ackOk = waitForAck(seqCounter);

  if (ackOk) {
    noAckCount = 0;
    lastSuccessfulSendTime = millis();
    Serial.println("ACK: OK — data delivered");
  } else {
    noAckCount++;
    Serial.println("ACK: MISS (" + String(noAckCount) + ")");
    // Retry once
    if (noAckCount <= 3) {
      // Random 1-3s backoff delay to recover from LoRa collisions
      unsigned long retryDelay = random(1000, 3001);
      Serial.println("RETRY backoff: " + String(retryDelay) + "ms");
      delay(retryDelay);
      sendLoRa(payload);
      ackOk = waitForAck(seqCounter);
      if (ackOk) {
        noAckCount = 0;
        lastSuccessfulSendTime = millis();
        Serial.println("RETRY: OK — data delivered");
      } else {
        Serial.println("RETRY: MISS");
      }
    }
  }

  updateLED();
}
