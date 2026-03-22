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
// WIFI CREDENTIALS
// =====================================================
const char* STA_SSID = "IKYK";
const char* STA_PASS = "444everydayOK*";

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
const unsigned long GPS_CACHE_MAX_AGE_MS = 300000;

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
const unsigned long NETWORK_OK_MS   = 30000;
const int MAX_HOPS = 3;

// =====================================================
// GATEWAY / LORA DISCOVERY
// =====================================================
const unsigned long BEACON_TIMEOUT_MS  = 45000;
const unsigned long STARTUP_LISTEN_MS  = 6000;
unsigned long lastGatewayContact       = 0;
bool gatewayReachable                  = false;
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
bool loraInitOk = false;

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
  gpsSerial.println("$PMTK220,1000*1F");   delay(100);
  gpsSerial.println("$PMTK314,0,1,0,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0*28"); delay(100);
  gpsSerial.println("$PMTK313,1*2E");       delay(100);
  gpsSerial.println("$PMTK301,2*2E");       delay(100);
  gpsSerial.println("$PMTK286,1*23");       delay(100);
  gpsSerial.println("$PMTK869,1,1*35");     delay(100);
  gpsSerial.println("$PMTK886,1*25");       delay(100);
  Serial.println("GPS: init commands sent");
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

String fireStatus(int flameDigital) {
  if (flameDigital != LOW && flameDigital != HIGH) return "Needs replacement";
  return (flameDigital == LOW) ? "Flame" : "None";
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
// =====================================================
String classifyCategory(String tempStr, String humStr, String smokeStr, String fireStr, String healthStr) {
  if (healthStr.indexOf("Needs replacement") >= 0) return "Warning";
  float temp = tempStr.toFloat();
  float hum  = humStr.toFloat();
  int smoke  = smokeStr.toInt();
  bool flame = (fireStr == "Flame");
  int fireVotes = 0, warningVotes = 0;

  if (!isReplacementValue(tempStr)) {
    if (temp >= 58.0) fireVotes++; else if (temp >= 39.0) warningVotes++;
  }
  if (!isReplacementValue(smokeStr)) {
    if (smoke >= 850) fireVotes++; else if (smoke >= 450) warningVotes++;
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
//
// SOLID         = Connected & healthy (WiFi or LoRa)
// SLOW BLINK    = Sensor needs replacement
// FAST BLINK    = No network, searching for connection
// MEDIUM BLINK  = LoRa mode (actively using LoRa)
// =====================================================
void updateLED() {
  // Read sensors for maintenance check
  float temp = dht.readTemperature();
  float hum  = dht.readHumidity();
  int mq2    = analogRead(MQ2_PIN);
  int flame  = digitalRead(FLAME_PIN);
  bool maintenance = needsMaintenance(temp, hum, mq2, flame);

  bool networkOk = wifiConnected || ((millis() - lastAckTime) < NETWORK_OK_MS);

  // SLOW BLINK = Sensor problem
  if (maintenance) {
    if (millis() - lastLedBlink >= 800) {
      lastLedBlink = millis();
      ledState = !ledState;
      digitalWrite(LED_PIN, ledState);
    }
  }
  // FAST BLINK = No network, still searching
  else if (!networkOk && !wifiConnected) {
    if (millis() - lastLedBlink >= 150) {
      lastLedBlink = millis();
      ledState = !ledState;
      digitalWrite(LED_PIN, ledState);
    }
  }
  // SOLID = WiFi connected & healthy
  else if (wifiConnected) {
    digitalWrite(LED_PIN, HIGH);
    ledState = true;
  }
  // MEDIUM BLINK = LoRa mode
  else if (!wifiConnected && networkOk) {
    if (millis() - lastLedBlink >= 400) {
      lastLedBlink = millis();
      ledState = !ledState;
      digitalWrite(LED_PIN, ledState);
    }
  }
  // SOLID = Signal healthy + connected
  else {
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
      lastGatewayContact = millis(); gatewayReachable = true; continue;
    }
    if (type == "ACK" && key == NODE_PASSKEY) {
      String dest = getField(rx, 2);
      String seq  = getField(rx, 3);
      if (dest.toInt() == NODE_ID && seq.toInt() == (int)mySeq) {
        lastAckTime = millis(); lastGatewayContact = millis();
        gatewayReachable = true; noAckCount = 0;
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

  String category = classifyCategory(tempStr, humStr, mq2Str, fireStr, healthStr);

  HTTPClient http;
  WiFiClientSecure client;
  client.setInsecure();

  String url = String(FIREBASE_HOST) + "/nodes/node" + String(originNode) + ".json?auth=" + String(FIREBASE_AUTH);

  // Determine barangay from node assignment (same mapping as gateway)
  String brgy = NODE_BARANGAY;  // Default — will be overridden if we know the mapping
  // Use a simple lookup since we know the 4 nodes
  // In production, this could be a config array
  // For now, use the same barangay — the gateway has the canonical mapping

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
    Serial.println("BEACON from gateway");
    return;
  }

  // ACK for us
  if (type == "ACK") {
    String dest = getField(rx, 2);
    if (dest.toInt() == NODE_ID) {
      lastAckTime = millis(); lastGatewayContact = millis();
      gatewayReachable = true; noAckCount = 0;
    }
    return;
  }

  if (type != "DATA") return;

  // Validate passkey — must be a valid CENSYS node
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
    Serial.println("WiFi RELAY: received N" + originStr + " via LoRa → pushing to Firebase");
    pushOtherNodeToFirebase(origin, key, tempStr, humStr, mq2Str,
                             fireStr, latStr, lngStr, healthStr, pathStr);
    return;
  }

  // ============================================
  // LoRa mode: relay via LoRa to gateway
  // ============================================
  if (!gatewayReachable) return;
  if (hops >= MAX_HOPS) return;
  if (isNodeInPath(pathStr, NODE_ID)) return;

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

  HTTPClient http;
  WiFiClientSecure client;
  client.setInsecure();

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
    Serial.println("Firebase push: HTTP " + String(httpCode));
    lastAckTime = millis();
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
  Serial.println("  Barangay: " + NODE_BARANGAY);
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
    LoRa.setSpreadingFactor(10);
    LoRa.setSignalBandwidth(125E3);
    LoRa.setCodingRate4(8);
    LoRa.setTxPower(20, PA_OUTPUT_PA_BOOST_PIN);
    LoRa.setPreambleLength(8);
    LoRa.enableCrc();
    LoRa.setGain(0);
    LoRa.receive();
    loraInitOk = true;
    Serial.println("LoRa: SF10, BW125k, CR4/8, TX20dBm");
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
          gatewayReachable = true;
          Serial.println(">>> Gateway FOUND <<<");
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
  } else if (gatewayReachable) {
    Serial.println("MODE: LoRa → Gateway (LED: MEDIUM BLINK)");
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
      String category = classifyCategory(tempStr, humStr, mq2Str, fireStr, healthStr);

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

  // Gateway reachability check
  if (gatewayReachable) {
    if ((millis() - lastGatewayContact > BEACON_TIMEOUT_MS) && (noAckCount >= RELAY_MODE_ACK_THRESHOLD)) {
      gatewayReachable = false;
      Serial.println("GATEWAY LOST");
    }
  } else {
    if (millis() - lastGatewayContact < BEACON_TIMEOUT_MS && noAckCount == 0) {
      gatewayReachable = true;
      Serial.println(">>> Gateway RECOVERED <<<");
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
  String fireStr = isFlameBad(flameVal) ? "Needs replacement" : fireStatus(flameVal);
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
  Serial.println("GW:" + String(gatewayReachable ? "DIRECT" : "RELAY→NEAREST"));

  sendLoRa(payload);
  bool ackOk = waitForAck(seqCounter);

  if (ackOk) {
    noAckCount = 0;
    Serial.println("ACK: OK");
  } else {
    noAckCount++;
    Serial.println("ACK: MISS (" + String(noAckCount) + ")");
    // Retry once
    if (noAckCount <= 3) {
      delay(200 + NODE_ID * 50);
      sendLoRa(payload);
      ackOk = waitForAck(seqCounter);
      if (ackOk) { noAckCount = 0; Serial.println("RETRY: OK"); }
      else Serial.println("RETRY: MISS");
    }
  }

  updateLED();
}
