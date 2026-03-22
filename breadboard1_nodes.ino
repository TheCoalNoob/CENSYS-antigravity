#include <SPI.h>
#include <LoRa.h>
#include <DHT.h>
#include <TinyGPSPlus.h>

// =====================================================
// NODE SETTINGS (CHANGE PER NODE)
// =====================================================
#define NODE_ID 4
String NODE_PASSKEY = "CENSYS_N4_2026";   // change per node

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
// GPS CACHED COORDINATES (for indoor use)
// =====================================================
double cachedLat = 0.0;
double cachedLng = 0.0;
bool hasCachedGPS = false;
unsigned long lastGPSFixMillis = 0;
const unsigned long GPS_CACHE_MAX_AGE_MS = 300000;

// =====================================================
// TDMA TIMING — Each node gets a 3-second slot
// in a 12-second cycle. ZERO collisions.
//
//  Cycle:  |---N1---|---N2---|---N3---|---N4---|
//  Time:   0s      3s      6s      9s     12s
//
// =====================================================
const unsigned long TDMA_CYCLE_MS   = 12000;  // Full cycle = 12 seconds
const unsigned long TDMA_SLOT_MS    = 3000;   // Each node gets 3 seconds
const unsigned long ACK_TIMEOUT_MS  = 1500;   // ACK wait (generous for mesh hops)
const unsigned long NETWORK_OK_MS   = 30000;  // 30s = 2.5 missed cycles before "no network"
const int MAX_HOPS = 3;

// =====================================================
// GATEWAY DISCOVERY / BEACON
// =====================================================
const unsigned long BEACON_TIMEOUT_MS  = 45000;  // 45s no beacon/ACK = gateway unreachable
const unsigned long STARTUP_LISTEN_MS  = 10000;  // Listen for gateway on boot (10 seconds)
unsigned long lastGatewayContact       = 0;       // Last beacon OR ACK from gateway
bool gatewayReachable                  = false;
unsigned long bootMillis               = 0;

// Relay mode requires BOTH conditions:
// 1. No ACK for 5+ consecutive sends
// 2. No gateway contact for BEACON_TIMEOUT_MS
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

// Track our TDMA cycle start time
unsigned long cycleStartMillis = 0;

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

// =====================================================
// CHECK IF NODE_ID IS IN PATH (circular relay prevention)
// Path format: "1" or "1>3" or "1>3>2"
// =====================================================
bool isNodeInPath(String path, int nodeId) {
  String nodeStr = String(nodeId);
  int pathLen = path.length();
  int nodeStrLen = nodeStr.length();

  for (int i = 0; i <= pathLen - nodeStrLen; i++) {
    bool match = true;
    for (int j = 0; j < nodeStrLen; j++) {
      if (path.charAt(i + j) != nodeStr.charAt(j)) {
        match = false;
        break;
      }
    }
    if (match) {
      // Check that it's a whole token (bounded by start/end or '>')
      bool leftOk = (i == 0) || (path.charAt(i - 1) == '>');
      bool rightOk = (i + nodeStrLen >= pathLen) || (path.charAt(i + nodeStrLen) == '>');
      if (leftOk && rightOk) return true;
    }
  }
  return false;
}

// =====================================================
// LED STATUS
// =====================================================
void updateLED(float temp, float hum, int mq2, int flameVal) {
  bool maintenance = needsMaintenance(temp, hum, mq2, flameVal);
  bool networkOk = (millis() - lastAckTime) < NETWORK_OK_MS;

  if (maintenance) {
    // Slow blink = sensor needs replacement
    if (millis() - lastLedBlink >= 800) {
      lastLedBlink = millis();
      ledState = !ledState;
      digitalWrite(LED_PIN, ledState);
    }
  } else if (!networkOk) {
    // Fast blink = no network
    if (millis() - lastLedBlink >= 180) {
      lastLedBlink = millis();
      ledState = !ledState;
      digitalWrite(LED_PIN, ledState);
    }
  } else if (!gatewayReachable) {
    // Medium blink = in relay mode
    if (millis() - lastLedBlink >= 400) {
      lastLedBlink = millis();
      ledState = !ledState;
      digitalWrite(LED_PIN, ledState);
    }
  } else {
    // Solid = connected to gateway directly
    digitalWrite(LED_PIN, HIGH);
    ledState = true;
  }
}

// =====================================================
// FEED GPS
// =====================================================
void feedGPS() {
  while (gpsSerial.available()) {
    gps.encode(gpsSerial.read());
  }
}

// =====================================================
// LORA SEND — blocking send with receive restore
// Using blocking mode for reliable transmission
// =====================================================
void sendLoRa(String packet) {
  LoRa.idle();
  LoRa.beginPacket();
  LoRa.print(packet);
  LoRa.endPacket();      // BLOCKING — waits until fully sent
  delay(20);
  LoRa.receive();
}

// =====================================================
// TDMA SLOT — Check if it's this node's turn to send
// =====================================================
bool isMySlot() {
  unsigned long cycleTime = millis() % TDMA_CYCLE_MS;
  unsigned long mySlotStart = (unsigned long)(NODE_ID - 1) * TDMA_SLOT_MS;
  unsigned long mySlotEnd = mySlotStart + TDMA_SLOT_MS;
  return (cycleTime >= mySlotStart && cycleTime < mySlotEnd);
}

// Returns milliseconds until this node's next slot
unsigned long msUntilMySlot() {
  unsigned long cycleTime = millis() % TDMA_CYCLE_MS;
  unsigned long mySlotStart = (unsigned long)(NODE_ID - 1) * TDMA_SLOT_MS;

  if (cycleTime < mySlotStart) {
    return mySlotStart - cycleTime;
  } else if (cycleTime < mySlotStart + TDMA_SLOT_MS) {
    return 0;  // We're in our slot right now
  } else {
    // Next cycle
    return (TDMA_CYCLE_MS - cycleTime) + mySlotStart;
  }
}

// =====================================================
// WAIT FOR ACK — with GPS feeding and beacon detection
// =====================================================
bool waitForAck(unsigned long mySeq) {
  unsigned long t0 = millis();
  while (millis() - t0 < ACK_TIMEOUT_MS) {
    feedGPS();
    int packetSize = LoRa.parsePacket();
    if (!packetSize) {
      delay(5);
      continue;
    }
    String rx = "";
    while (LoRa.available()) rx += (char)LoRa.read();
    rx.trim();

    String key  = getField(rx, 0);
    String type = getField(rx, 1);

    // Beacon received during ACK wait
    if (type == "BEACON" && key == "CENSYS_GW") {
      lastGatewayContact = millis();
      gatewayReachable = true;
      continue;
    }

    // ACK for us
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
// PROCESS INCOMING — handle beacons, ACKs, and relay
//
// KEY RULES:
// 1. If gateway is reachable → DO NOT RELAY (no need)
// 2. Only relay if this node can reach gateway AND
//    the origin node apparently can't
// 3. NEVER relay if own NODE_ID is in the path
//    (prevents circular relay loops)
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

  // ===== GATEWAY BEACON =====
  if (type == "BEACON" && key == "CENSYS_GW") {
    lastGatewayContact = millis();
    gatewayReachable = true;
    Serial.println("BEACON from gateway");
    return;
  }

  // ===== ACK for this node =====
  if (type == "ACK") {
    String dest = getField(rx, 2);
    if (dest.toInt() == NODE_ID) {
      lastAckTime = millis();
      lastGatewayContact = millis();
      gatewayReachable = true;
      noAckCount = 0;
      Serial.println("ACK received (from processIncoming)");
    }
    return;
  }

  // ===== DATA PACKET — potential relay =====
  if (type != "DATA") return;

  // Validate passkey
  if (!(key == "CENSYS_N1_2026" || key == "CENSYS_N2_2026" ||
        key == "CENSYS_N3_2026" || key == "CENSYS_N4_2026")) {
    return;
  }

  String originStr = getField(rx, 2);
  String senderStr = getField(rx, 3);
  String seqStr    = getField(rx, 4);
  String hopsStr   = getField(rx, 5);
  String pathStr   = getField(rx, 13);

  int origin = originStr.toInt();
  int hops   = hopsStr.toInt();

  // Don't process our own packets
  if (origin == NODE_ID) return;

  // Deduplicate
  String sig = key + "|" + originStr + "|" + seqStr;
  if (isSeen(sig)) return;
  addSeen(sig);

  // ===== RELAY DECISION =====
  // Rule 1: Only relay if THIS node can reach the gateway
  if (!gatewayReachable) {
    Serial.println("SKIP relay N" + originStr + " — can't reach gateway ourselves");
    return;
  }

  // Rule 2: Don't relay if max hops exceeded
  if (hops >= MAX_HOPS) {
    Serial.println("SKIP relay N" + originStr + " — max hops (" + String(hops) + ")");
    return;
  }

  // Rule 3: CIRCULAR RELAY PREVENTION
  // If our NODE_ID is already in the path, don't relay (prevents N2→N3→N2 loops)
  if (isNodeInPath(pathStr, NODE_ID)) {
    Serial.println("SKIP relay N" + originStr + " — circular: our ID is in path: " + pathStr);
    return;
  }

  // All checks passed — relay this packet
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

  // Wait for a clear channel moment (small delay based on NODE_ID)
  delay(100 + NODE_ID * 50);
  sendLoRa(relayPacket);

  Serial.println("RELAYED N" + originStr + " (hops:" + String(hops + 1) + ") path:" + newPath);
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
        cachedLat = newLat;
        cachedLng = newLng;
        hasCachedGPS = true;
        lastGPSFixMillis = millis();
      } else if (!hasCachedGPS) {
        cachedLat = newLat;
        cachedLng = newLng;
        hasCachedGPS = true;
        lastGPSFixMillis = millis();
      }
    }
  }
}

bool getGPSCoordinates(double &lat, double &lng, String &gpsStatus) {
  if (gps.location.isValid()) {
    lat = gps.location.lat();
    lng = gps.location.lng();
    gpsStatus = "Live";
    return true;
  }
  if (hasCachedGPS && (millis() - lastGPSFixMillis) < GPS_CACHE_MAX_AGE_MS) {
    lat = cachedLat;
    lng = cachedLng;
    gpsStatus = "Cached";
    return true;
  }
  lat = 0.0;
  lng = 0.0;
  gpsStatus = "No Fix";
  return false;
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

  // LoRa init
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);
  if (!LoRa.begin(433E6)) {
    Serial.println("LoRa init failed!");
    while (1) {
      digitalWrite(LED_PIN, !digitalRead(LED_PIN));
      delay(120);
    }
  }

  // ===== LORA SETTINGS =====
  LoRa.setSpreadingFactor(10);
  LoRa.setSignalBandwidth(125E3);
  LoRa.setCodingRate4(8);
  LoRa.setTxPower(20, PA_OUTPUT_PA_BOOST_PIN);
  LoRa.setPreambleLength(8);
  LoRa.enableCrc();
  LoRa.setGain(0);

  Serial.println("LoRa: SF10, BW125k, CR4/8, TX20dBm, CRC ON");
  LoRa.receive();

  randomSeed(analogRead(35) ^ (NODE_ID * 12345));
  bootMillis = millis();

  // ===== GATEWAY DISCOVERY PHASE =====
  Serial.println("========================================");
  Serial.println("  CENSYS Node " + String(NODE_ID) + " — Scanning for gateway...");
  Serial.println("========================================");

  unsigned long scanStart = millis();
  while (millis() - scanStart < STARTUP_LISTEN_MS) {
    feedGPS();
    int packetSize = LoRa.parsePacket();
    if (packetSize) {
      String rx = "";
      while (LoRa.available()) rx += (char)LoRa.read();
      rx.trim();
      String type = getField(rx, 1);
      String key  = getField(rx, 0);

      if (type == "BEACON" && key == "CENSYS_GW") {
        lastGatewayContact = millis();
        lastAckTime = millis();  // Treat beacon as initial contact
        gatewayReachable = true;
        Serial.println(">>> Gateway FOUND! <<<");
        break;
      }
    }
    delay(10);
  }

  if (!gatewayReachable) {
    Serial.println("Gateway not found in " + String(STARTUP_LISTEN_MS/1000) + "s scan");
    Serial.println("Will keep listening and enter relay mode if needed");
  }

  // Calculate when our first TDMA slot starts
  // Give each node some initial offset to avoid boot-time collisions
  cycleStartMillis = millis();

  Serial.println("----------------------------------------");
  Serial.println("Node " + String(NODE_ID) + " READY");
  Serial.println("TDMA slot: " + String((NODE_ID-1)*3) + "s - " + String(NODE_ID*3) + "s in 12s cycle");
  Serial.println("Gateway: " + String(gatewayReachable ? "CONNECTED" : "NOT FOUND"));
  Serial.println("----------------------------------------");
}

// =====================================================
// LOOP
// =====================================================
void loop() {
  feedGPS();
  updateGPSCache();

  // ===== UPDATE GATEWAY REACHABILITY =====
  // Gateway is unreachable if we haven't heard ANY contact (beacon or ACK)
  // for BEACON_TIMEOUT_MS AND we've had enough consecutive ACK failures
  if (gatewayReachable) {
    if ((millis() - lastGatewayContact > BEACON_TIMEOUT_MS) &&
        (noAckCount >= RELAY_MODE_ACK_THRESHOLD)) {
      gatewayReachable = false;
      Serial.println("========================================");
      Serial.println("  GATEWAY LOST — Relay mode active");
      Serial.println("  No contact for " + String(BEACON_TIMEOUT_MS/1000) + "s");
      Serial.println("  ACK failures: " + String(noAckCount));
      Serial.println("========================================");
    }
  } else {
    // Check if gateway came back (via beacon/ACK in processIncoming)
    if (millis() - lastGatewayContact < BEACON_TIMEOUT_MS && noAckCount == 0) {
      gatewayReachable = true;
      Serial.println(">>> Gateway RECOVERED! <<<");
    }
  }

  // ===== PROCESS INCOMING (beacons, ACKs, relay packets) =====
  processIncoming();

  // ===== TDMA: Only send during our time slot =====
  if (!isMySlot()) {
    // Not our turn — just listen and process incoming
    static unsigned long lastLedUpdate = 0;
    if (millis() - lastLedUpdate >= 500) {
      lastLedUpdate = millis();
      float t = dht.readTemperature();
      float h = dht.readHumidity();
      int s = analogRead(MQ2_PIN);
      int f = digitalRead(FLAME_PIN);
      updateLED(t, h, s, f);
    }
    return;
  }

  // ===== IN OUR SLOT — But only send once per cycle =====
  unsigned long currentCycle = millis() / TDMA_CYCLE_MS;
  static unsigned long lastSentCycle = 0;

  if (currentCycle == lastSentCycle) {
    // Already sent this cycle
    return;
  }
  lastSentCycle = currentCycle;

  // ===== SEND DATA =====
  seqCounter++;

  float temp = dht.readTemperature();
  float hum  = dht.readHumidity();
  int mq2Raw = analogRead(MQ2_PIN);
  int flameVal = digitalRead(FLAME_PIN);

  double lat = 0.0, lng = 0.0;
  String gpsStatus = "No Fix";
  bool hasGPS = getGPSCoordinates(lat, lng, gpsStatus);
  bool gpsLive = gps.location.isValid();
  bool gpsCached = (hasGPS && !gpsLive);

  int satCount = gps.satellites.isValid() ? gps.satellites.value() : 0;

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

  Serial.println("==== TX [SLOT " + String(NODE_ID) + "] Seq:" + String(seqCounter) + " ====");
  Serial.println("T:" + tempStr + " H:" + humStr + " S:" + mq2Str + " F:" + fireStr);
  Serial.println("GPS:" + gpsStatus + " Sat:" + String(satCount));
  Serial.println("GW:" + String(gatewayReachable ? "DIRECT" : "RELAY"));

  sendLoRa(payload);
  bool ackOk = waitForAck(seqCounter);

  if (ackOk) {
    noAckCount = 0;
    Serial.println("ACK: OK (direct from gateway)");
  } else {
    noAckCount++;
    Serial.println("ACK: MISS (" + String(noAckCount) + " consecutive)");

    // ONE retry within our slot window (we still have time)
    if (noAckCount <= 3) {
      delay(200 + NODE_ID * 50);
      Serial.println("RETRY...");
      sendLoRa(payload);
      ackOk = waitForAck(seqCounter);
      if (ackOk) {
        noAckCount = 0;
        Serial.println("RETRY ACK: OK");
      } else {
        Serial.println("RETRY ACK: MISS");
      }
    }
  }

  // Update LED
  updateLED(temp, hum, mq2Raw, flameVal);
}
