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
String NODE_BARANGAY = "kalunasan";        // change per node

// =====================================================
// WIFI CREDENTIALS (same as gateway)
// =====================================================
const char* STA_SSID = "IKYK";
const char* STA_PASS = "444everydayOK*";

// =====================================================
// FIREBASE CONFIG (same as gateway — pushes to same DB)
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
const unsigned long GPS_CACHE_MAX_AGE_MS = 300000;

// =====================================================
// WIFI STATE
// =====================================================
bool wifiConnected = false;
const unsigned long WIFI_CONNECT_TIMEOUT_MS = 8000;   // 8s to try WiFi on boot
const unsigned long WIFI_RETRY_INTERVAL_MS  = 60000;  // Retry WiFi every 60s if lost
unsigned long lastWifiRetry = 0;
const unsigned long WIFI_SEND_INTERVAL_MS   = 10000;  // Firebase push every 10s via WiFi

// =====================================================
// TDMA TIMING (used only in LoRa mode)
// =====================================================
const unsigned long TDMA_CYCLE_MS   = 12000;
const unsigned long TDMA_SLOT_MS    = 3000;
const unsigned long ACK_TIMEOUT_MS  = 1500;
const unsigned long NETWORK_OK_MS   = 30000;
const int MAX_HOPS = 3;

// =====================================================
// GATEWAY DISCOVERY / BEACON (LoRa mode only)
// =====================================================
const unsigned long BEACON_TIMEOUT_MS  = 45000;
const unsigned long STARTUP_LISTEN_MS  = 6000;  // Shorter since WiFi already tried
unsigned long lastGatewayContact       = 0;
bool gatewayReachable                  = false;
unsigned long bootMillis               = 0;
const int RELAY_MODE_ACK_THRESHOLD = 5;

// =====================================================
// STATE
// =====================================================
unsigned long lastSendTime = 0;
unsigned long lastAckTime = 0;
unsigned long lastLedBlink = 0;
bool ledState = false;
unsigned long seqCounter = 0;
int noAckCount = 0;

// =====================================================
// DUPLICATE TRACKER
// =====================================================
const int SEEN_MAX = 30;
String seenPackets[SEEN_MAX];
int seenIndex = 0;

// =====================================================
// GPS INITIALIZATION
// =====================================================
void initGPSModule() {
  gpsSerial.println("$PMTK220,1000*1F");
  delay(100);
  gpsSerial.println("$PMTK314,0,1,0,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0*28");
  delay(100);
  gpsSerial.println("$PMTK313,1*2E");
  delay(100);
  gpsSerial.println("$PMTK301,2*2E");
  delay(100);
  gpsSerial.println("$PMTK286,1*23");
  delay(100);
  gpsSerial.println("$PMTK869,1,1*35");
  delay(100);
  gpsSerial.println("$PMTK886,1*25");
  delay(100);
  Serial.println("GPS: PMTK init commands sent");
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
  for (int i = 0; i < SEEN_MAX; i++) {
    if (seenPackets[i] == sig) return true;
  }
  return false;
}

void addSeen(String sig) {
  seenPackets[seenIndex] = sig;
  seenIndex = (seenIndex + 1) % SEEN_MAX;
}

String fireStatus(int flameDigital) {
  if (flameDigital != LOW && flameDigital != HIGH) return "Needs replacement";
  return (flameDigital == LOW) ? "Flame" : "None";
}

bool isDHTBad(float temp, float hum) {
  if (isnan(temp) || isnan(hum)) return true;
  if (temp < -20 || temp > 80) return true;
  if (hum < 0 || hum > 100) return true;
  return false;
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
  if (issues.length() == 0) return "OK";
  return issues;
}

bool needsMaintenance(float temp, float hum, int mq2, int flameVal) {
  return isDHTBad(temp, hum) || isMQ2Bad(mq2) || isFlameBad(flameVal);
}

String esc(String s) {
  s.replace("\\", "\\\\");
  s.replace("\"", "\\\"");
  s.replace("\n", " ");
  s.replace("\r", " ");
  return s;
}

bool isReplacementValue(String v) {
  v.trim();
  return (v == "Needs replacement" || v.length() == 0);
}

// Circular relay prevention
bool isNodeInPath(String path, int nodeId) {
  String nodeStr = String(nodeId);
  int pathLen = path.length();
  int nodeStrLen = nodeStr.length();
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
// CATEGORY CLASSIFICATION
// Same logic as gateway — needed for WiFi direct push
// =====================================================
String classifyCategory(String tempStr, String humStr, String smokeStr, String fireStr, String healthStr) {
  if (healthStr.indexOf("Needs replacement") >= 0) return "Warning";

  float temp = tempStr.toFloat();
  float hum  = humStr.toFloat();
  int smoke  = smokeStr.toInt();
  bool flame = (fireStr == "Flame");

  int fireVotes = 0, warningVotes = 0;

  if (!isReplacementValue(tempStr)) {
    if (temp >= 58.0) fireVotes++;
    else if (temp >= 39.0) warningVotes++;
  }
  if (!isReplacementValue(smokeStr)) {
    if (smoke >= 850) fireVotes++;
    else if (smoke >= 450) warningVotes++;
  }
  if (!isReplacementValue(fireStr) && flame) fireVotes++;

  if (!isReplacementValue(tempStr) && !isReplacementValue(humStr)) {
    if (temp >= 39.0 && hum <= 25.0) warningVotes++;
    if (temp >= 45.0 && hum <= 20.0) warningVotes++;
  }

  if (fireVotes >= 2) return "Fire";
  if (fireVotes >= 1 && warningVotes >= 1) return "Fire";
  if (flame && smoke >= 450) return "Fire";
  if (flame && temp >= 39.0) return "Fire";
  if (temp >= 65.0) return "Fire";
  if (fireVotes == 1) return "Warning";
  if (warningVotes >= 1) return "Warning";
  return "Normal";
}

// =====================================================
// LED STATUS
// =====================================================
void updateLED(float temp, float hum, int mq2, int flameVal) {
  bool maintenance = needsMaintenance(temp, hum, mq2, flameVal);
  bool networkOk = wifiConnected || ((millis() - lastAckTime) < NETWORK_OK_MS);

  if (maintenance) {
    if (millis() - lastLedBlink >= 800) { lastLedBlink = millis(); ledState = !ledState; digitalWrite(LED_PIN, ledState); }
  } else if (!networkOk) {
    if (millis() - lastLedBlink >= 180) { lastLedBlink = millis(); ledState = !ledState; digitalWrite(LED_PIN, ledState); }
  } else if (wifiConnected) {
    // Double blink = WiFi mode (blink-blink-pause)
    unsigned long phase = (millis() / 150) % 6;
    digitalWrite(LED_PIN, (phase == 0 || phase == 2) ? HIGH : LOW);
  } else if (!gatewayReachable) {
    if (millis() - lastLedBlink >= 400) { lastLedBlink = millis(); ledState = !ledState; digitalWrite(LED_PIN, ledState); }
  } else {
    digitalWrite(LED_PIN, HIGH);
    ledState = true;
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
      lastGatewayContact = millis();
      gatewayReachable = true;
      continue;
    }

    if (type == "ACK" && key == NODE_PASSKEY) {
      String dest = getField(rx, 2);
      String seq  = getField(rx, 3);
      if (dest.toInt() == NODE_ID && seq.toInt() == (int)mySeq) {
        lastAckTime = millis();
        lastGatewayContact = millis();
        gatewayReachable = true;
        noAckCount = 0;
        return true;
      }
    }
  }
  return false;
}

// =====================================================
// PROCESS INCOMING (relay for mesh — LoRa mode only)
// =====================================================
void processIncoming() {
  feedGPS();
  int packetSize = LoRa.parsePacket();
  if (!packetSize) return;

  String rx = "";
  while (LoRa.available()) rx += (char)LoRa.read();
  rx.trim();
  if (rx.length() < 5) return;

  String key  = getField(rx, 0);
  String type = getField(rx, 1);

  if (type == "BEACON" && key == "CENSYS_GW") {
    lastGatewayContact = millis();
    gatewayReachable = true;
    Serial.println("BEACON from gateway");
    return;
  }

  if (type == "ACK") {
    String dest = getField(rx, 2);
    if (dest.toInt() == NODE_ID) {
      lastAckTime = millis();
      lastGatewayContact = millis();
      gatewayReachable = true;
      noAckCount = 0;
    }
    return;
  }

  if (type != "DATA") return;
  if (!(key == "CENSYS_N1_2026" || key == "CENSYS_N2_2026" ||
        key == "CENSYS_N3_2026" || key == "CENSYS_N4_2026")) return;

  String originStr = getField(rx, 2);
  String seqStr    = getField(rx, 4);
  String hopsStr   = getField(rx, 5);
  String pathStr   = getField(rx, 13);

  int origin = originStr.toInt();
  int hops   = hopsStr.toInt();

  if (origin == NODE_ID) return;

  String sig = key + "|" + originStr + "|" + seqStr;
  if (isSeen(sig)) return;
  addSeen(sig);

  if (!gatewayReachable) return;
  if (hops >= MAX_HOPS) return;
  if (isNodeInPath(pathStr, NODE_ID)) return;

  String tempStr   = getField(rx, 6);
  String humStr    = getField(rx, 7);
  String mq2Str    = getField(rx, 8);
  String fireStr   = getField(rx, 9);
  String latStr    = getField(rx, 10);
  String lngStr    = getField(rx, 11);
  String healthStr = getField(rx, 12);

  String newPath = pathStr + ">" + String(NODE_ID);
  String relayPacket =
    key + "|DATA|" + originStr + "|" + String(NODE_ID) + "|" +
    seqStr + "|" + String(hops + 1) + "|" +
    tempStr + "|" + humStr + "|" + mq2Str + "|" + fireStr + "|" +
    latStr + "|" + lngStr + "|" + healthStr + "|" + newPath;

  delay(100 + NODE_ID * 50);
  sendLoRa(relayPacket);
  Serial.println("RELAYED N" + originStr + " (hops:" + String(hops + 1) + ")");
}

// =====================================================
// GPS CACHE
// =====================================================
void updateGPSCache() {
  feedGPS();
  if (gps.location.isValid() && gps.location.isUpdated()) {
    double newLat = gps.location.lat();
    double newLng = gps.location.lng();
    if (newLat > 4.0 && newLat < 22.0 && newLng > 116.0 && newLng < 128.0) {
      if (gps.hdop.isValid() && gps.hdop.hdop() < 5.0) {
        cachedLat = newLat; cachedLng = newLng; hasCachedGPS = true; lastGPSFixMillis = millis();
      } else if (!hasCachedGPS) {
        cachedLat = newLat; cachedLng = newLng; hasCachedGPS = true; lastGPSFixMillis = millis();
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
// WIFI — Try to connect
// =====================================================
bool tryWiFiConnect() {
  Serial.print("WiFi: Connecting to ");
  Serial.println(STA_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(STA_SSID, STA_PASS);

  unsigned long startAttempt = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - startAttempt) < WIFI_CONNECT_TIMEOUT_MS) {
    delay(200);
    feedGPS();
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi: CONNECTED! IP: " + WiFi.localIP().toString());
    wifiConnected = true;
    return true;
  } else {
    Serial.println("WiFi: FAILED — will use LoRa TDMA instead");
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    wifiConnected = false;
    return false;
  }
}

// =====================================================
// FIREBASE PUSH (WiFi mode)
// Pushes to EXACT same path & format as gateway
// so the website sees no difference.
// =====================================================
void pushToFirebaseViaWiFi(String tempStr, String humStr, String mq2Str,
                            String fireStr, String latStr, String lngStr,
                            String healthStr, String category) {
  if (WiFi.status() != WL_CONNECTED) {
    wifiConnected = false;
    return;
  }

  HTTPClient http;
  WiFiClientSecure client;
  client.setInsecure();

  // Push to same path as gateway: /nodes/node{NODE_ID}
  String url = String(FIREBASE_HOST) + "/nodes/node" + String(NODE_ID) + ".json?auth=" + String(FIREBASE_AUTH);

  String json = "{";
  json += "\"online\":true,";
  json += "\"node\":" + String(NODE_ID) + ",";
  json += "\"barangay\":\"" + NODE_BARANGAY + "\",";
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
    Serial.println("Firebase WiFi push: HTTP " + String(httpCode));
    lastAckTime = millis();  // Treat successful push as "connected"
  } else {
    Serial.println("Firebase WiFi error: " + http.errorToString(httpCode));
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
  Serial.println("  Passkey: " + NODE_PASSKEY);
  Serial.println("  Barangay: " + NODE_BARANGAY);
  Serial.println("========================================");

  // ===== STEP 1: TRY WIFI FIRST =====
  Serial.println("STEP 1: Trying WiFi connection...");
  bool gotWifi = tryWiFiConnect();

  // ===== STEP 2: INIT LORA (always — needed for relay even in WiFi mode) =====
  Serial.println("STEP 2: Initializing LoRa...");
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);
  if (!LoRa.begin(433E6)) {
    Serial.println("LoRa init failed!");
    // If WiFi works, we can still operate without LoRa
    if (!gotWifi) {
      while (1) { digitalWrite(LED_PIN, !digitalRead(LED_PIN)); delay(120); }
    }
    Serial.println("WARNING: LoRa failed but WiFi is up — operating in WiFi-only mode");
  } else {
    LoRa.setSpreadingFactor(10);
    LoRa.setSignalBandwidth(125E3);
    LoRa.setCodingRate4(8);
    LoRa.setTxPower(20, PA_OUTPUT_PA_BOOST_PIN);
    LoRa.setPreambleLength(8);
    LoRa.enableCrc();
    LoRa.setGain(0);
    LoRa.receive();
    Serial.println("LoRa: SF10, BW125k, CR4/8, TX20dBm, CRC ON");
  }

  randomSeed(analogRead(35) ^ (NODE_ID * 12345));
  bootMillis = millis();

  // ===== STEP 3: IF NO WIFI, SCAN FOR GATEWAY BEACON =====
  if (!wifiConnected) {
    Serial.println("STEP 3: Scanning for gateway beacon...");
    unsigned long scanStart = millis();
    while (millis() - scanStart < STARTUP_LISTEN_MS) {
      feedGPS();
      int packetSize = LoRa.parsePacket();
      if (packetSize) {
        String rx = "";
        while (LoRa.available()) rx += (char)LoRa.read();
        rx.trim();
        if (getField(rx, 1) == "BEACON" && getField(rx, 0) == "CENSYS_GW") {
          lastGatewayContact = millis();
          lastAckTime = millis();
          gatewayReachable = true;
          Serial.println(">>> Gateway FOUND! <<<");
          break;
        }
      }
      delay(10);
    }
  }

  Serial.println("----------------------------------------");
  if (wifiConnected) {
    Serial.println("MODE: WiFi Direct → Firebase");
    Serial.println("IP: " + WiFi.localIP().toString());
    Serial.println("Push interval: " + String(WIFI_SEND_INTERVAL_MS / 1000) + "s");
  } else {
    Serial.println("MODE: LoRa TDMA → Gateway");
    Serial.println("TDMA slot: " + String((NODE_ID-1)*3) + "s - " + String(NODE_ID*3) + "s");
    Serial.println("Gateway: " + String(gatewayReachable ? "FOUND" : "NOT FOUND"));
  }
  Serial.println("----------------------------------------");
}

// =====================================================
// LOOP
// =====================================================
void loop() {
  feedGPS();
  updateGPSCache();

  // ===== CHECK WIFI STATUS =====
  if (wifiConnected && WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi: CONNECTION LOST — switching to LoRa TDMA");
    wifiConnected = false;
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    // Gateway might be reachable via LoRa
    LoRa.receive();
  }

  // ===== RETRY WIFI IF LOST =====
  if (!wifiConnected && (millis() - lastWifiRetry > WIFI_RETRY_INTERVAL_MS)) {
    lastWifiRetry = millis();
    Serial.println("WiFi: Retrying connection...");
    if (tryWiFiConnect()) {
      Serial.println("WiFi: RECOVERED — switching back to WiFi mode");
    }
  }

  // =============================================================
  //  MODE A: WiFi Direct → Firebase
  //  Node pushes data directly to Firebase.
  //  Website/dashboard sees it as if gateway pushed it.
  // =============================================================
  if (wifiConnected) {
    // Still process incoming LoRa (help relay for other nodes)
    processIncoming();

    // Send data via WiFi at fixed interval
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
      String fireStr = isFlameBad(flameVal) ? "Needs replacement" : fireStatus(flameVal);
      String healthStr = sensorHealthText(temp, hum, mq2Raw, flameVal, gpsLive, gpsCached);

      // Classify category (same logic as gateway)
      String category = classifyCategory(tempStr, humStr, mq2Str, fireStr, healthStr);

      Serial.println("==== WiFi TX Seq:" + String(seqCounter) + " ====");
      Serial.println("T:" + tempStr + " H:" + humStr + " S:" + mq2Str + " F:" + fireStr);
      Serial.println("Cat:" + category + " GPS:" + gpsStatus);

      pushToFirebaseViaWiFi(tempStr, humStr, mq2Str, fireStr,
                             String(lat, 6), String(lng, 6),
                             healthStr, category);
    }

    // LED update
    static unsigned long lastLedW = 0;
    if (millis() - lastLedW >= 300) {
      lastLedW = millis();
      float t = dht.readTemperature(); float h = dht.readHumidity();
      int s = analogRead(MQ2_PIN); int f = digitalRead(FLAME_PIN);
      updateLED(t, h, s, f);
    }
    return;
  }

  // =============================================================
  //  MODE B: LoRa TDMA → Gateway
  //  Standard TDMA time-slotted transmission.
  // =============================================================

  // Update gateway reachability
  if (gatewayReachable) {
    if ((millis() - lastGatewayContact > BEACON_TIMEOUT_MS) &&
        (noAckCount >= RELAY_MODE_ACK_THRESHOLD)) {
      gatewayReachable = false;
      Serial.println("GATEWAY LOST — relay mode active");
    }
  } else {
    if (millis() - lastGatewayContact < BEACON_TIMEOUT_MS && noAckCount == 0) {
      gatewayReachable = true;
      Serial.println(">>> Gateway RECOVERED <<<");
    }
  }

  processIncoming();

  // Only send during our TDMA slot
  if (!isMySlot()) {
    static unsigned long lastLedL = 0;
    if (millis() - lastLedL >= 500) {
      lastLedL = millis();
      float t = dht.readTemperature(); float h = dht.readHumidity();
      int s = analogRead(MQ2_PIN); int f = digitalRead(FLAME_PIN);
      updateLED(t, h, s, f);
    }
    return;
  }

  // One send per cycle
  unsigned long currentCycle = millis() / TDMA_CYCLE_MS;
  static unsigned long lastSentCycle = 0;
  if (currentCycle == lastSentCycle) return;
  lastSentCycle = currentCycle;

  // SEND DATA via LoRa
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
  String fireStr = isFlameBad(flameVal) ? "Needs replacement" : fireStatus(flameVal);
  String healthStr = sensorHealthText(temp, hum, mq2Raw, flameVal, gpsLive, gpsCached);

  String payload =
    NODE_PASSKEY + "|DATA|" +
    String(NODE_ID) + "|" + String(NODE_ID) + "|" +
    String(seqCounter) + "|0|" +
    tempStr + "|" + humStr + "|" + mq2Str + "|" + fireStr + "|" +
    String(lat, 6) + "|" + String(lng, 6) + "|" +
    healthStr + "|" + String(NODE_ID);

  String sig = NODE_PASSKEY + "|" + String(NODE_ID) + "|" + String(seqCounter);
  addSeen(sig);

  Serial.println("==== LoRa TX [SLOT " + String(NODE_ID) + "] Seq:" + String(seqCounter) + " ====");
  Serial.println("T:" + tempStr + " H:" + humStr + " S:" + mq2Str + " F:" + fireStr);
  Serial.println("GW:" + String(gatewayReachable ? "DIRECT" : "RELAY"));

  sendLoRa(payload);
  bool ackOk = waitForAck(seqCounter);

  if (ackOk) {
    noAckCount = 0;
    Serial.println("ACK: OK");
  } else {
    noAckCount++;
    Serial.println("ACK: MISS (" + String(noAckCount) + " consecutive)");
    if (noAckCount <= 3) {
      delay(200 + NODE_ID * 50);
      sendLoRa(payload);
      ackOk = waitForAck(seqCounter);
      if (ackOk) { noAckCount = 0; Serial.println("RETRY ACK: OK"); }
      else Serial.println("RETRY ACK: MISS");
    }
  }

  updateLED(temp, hum, mq2Raw, flameVal);
}
