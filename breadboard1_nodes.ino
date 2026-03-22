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
// TIMING
// =====================================================
const unsigned long SEND_INTERVAL_MS = 8000;   // 8 seconds between sends (reduced channel load)
const unsigned long ACK_TIMEOUT_MS   = 1200;   // ACK wait time (more time for mesh hops)
const unsigned long NETWORK_OK_MS    = 25000;
const int MAX_HOPS = 3;

// =====================================================
// GATEWAY DISCOVERY / BEACON
// =====================================================
const unsigned long BEACON_TIMEOUT_MS  = 30000;  // 30s no beacon = gateway unreachable
const unsigned long STARTUP_LISTEN_MS  = 8000;   // Listen for gateway on boot (8 seconds)
unsigned long lastBeaconMillis         = 0;
bool gatewayReachable                  = false;
bool startupDiscoveryDone              = false;
unsigned long bootMillis               = 0;

// =====================================================
// STATE
// =====================================================
unsigned long lastSendTime = 0;
unsigned long lastAckTime = 0;
unsigned long lastLedBlink = 0;
bool ledState = false;
unsigned long seqCounter = 0;
int noAckCount = 0;  // Track consecutive failed ACKs for retry logic

// =====================================================
// DUPLICATE TRACKER
// =====================================================
const int SEEN_MAX = 30;
String seenPackets[SEEN_MAX];
int seenIndex = 0;

// =====================================================
// GPS INITIALIZATION COMMANDS
// =====================================================
void initGPSModule() {
  gpsSerial.println("$PMTK220,1000*1F");   // 1Hz update rate
  delay(100);
  gpsSerial.println("$PMTK314,0,1,0,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0*28");  // RMC+GGA+GSA+GSV
  delay(100);
  gpsSerial.println("$PMTK313,1*2E");       // Enable SBAS
  delay(100);
  gpsSerial.println("$PMTK301,2*2E");       // DGPS mode = WAAS
  delay(100);
  gpsSerial.println("$PMTK286,1*23");       // Enable AIC (active interference cancellation)
  delay(100);
  gpsSerial.println("$PMTK869,1,1*35");     // Enable EASY (predict orbits)
  delay(100);
  gpsSerial.println("$PMTK886,1*25");       // Balloon mode (better for stationary)
  delay(100);
  Serial.println("GPS: PMTK init commands sent");
}

// =====================================================
// HELPERS
// =====================================================
String getField(String data, int index) {
  int found = 0;
  int start = 0;
  int end = -1;
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

bool isMQ2Bad(int mq2) {
  return (mq2 < 0 || mq2 > 4095);
}

bool isFlameBad(int flameVal) {
  return (flameVal != LOW && flameVal != HIGH);
}

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
    // Medium blink = gateway not found, relaying through mesh
    if (millis() - lastLedBlink >= 400) {
      lastLedBlink = millis();
      ledState = !ledState;
      digitalWrite(LED_PIN, ledState);
    }
  } else {
    // Solid = all good
    digitalWrite(LED_PIN, HIGH);
    ledState = true;
  }
}

// =====================================================
// FEED GPS — call frequently to avoid missing data
// =====================================================
void feedGPS() {
  while (gpsSerial.available()) {
    gps.encode(gpsSerial.read());
  }
}

// =====================================================
// LORA SEND — non-blocking with receive restore
// =====================================================
void sendLoRa(String packet) {
  LoRa.idle();               // Switch to idle before transmitting
  LoRa.beginPacket();
  LoRa.print(packet);
  LoRa.endPacket(true);      // true = async/non-blocking send
  delay(100);                // Brief wait for transmission to start
  LoRa.receive();            // Back to receive mode
}

// =====================================================
// CHECK FOR BEACON — non-blocking check for gateway beacon
// =====================================================
bool checkForBeacon() {
  int packetSize = LoRa.parsePacket();
  if (!packetSize) return false;

  String rx = "";
  while (LoRa.available()) rx += (char)LoRa.read();
  rx.trim();

  String key  = getField(rx, 0);
  String type = getField(rx, 1);

  if (type == "BEACON" && key == "CENSYS_GW") {
    lastBeaconMillis = millis();
    gatewayReachable = true;
    return true;
  }

  // If it's an ACK while scanning, still process
  if (type == "ACK") {
    String dest = getField(rx, 2);
    if (dest.toInt() == NODE_ID) {
      lastAckTime = millis();
      lastBeaconMillis = millis();  // ACK proves gateway is alive
      gatewayReachable = true;
    }
  }

  return false;
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

    // Beacon received during ACK wait — great, note it
    if (type == "BEACON" && key == "CENSYS_GW") {
      lastBeaconMillis = millis();
      gatewayReachable = true;
      continue;
    }

    if (type == "ACK" && key == NODE_PASSKEY) {
      String dest = getField(rx, 2);
      String seq  = getField(rx, 3);
      if (dest.toInt() == NODE_ID && seq.toInt() == (int)mySeq) {
        lastAckTime = millis();
        lastBeaconMillis = millis();  // ACK proves gateway is alive
        gatewayReachable = true;
        noAckCount = 0;
        return true;
      }
    }
  }
  return false;
}

// =====================================================
// PROCESS INCOMING (relay for mesh)
// Smart relay: only relay if THIS node can reach
// the gateway (has recent beacon/ACK).
// Relay any packet from nodes that can't reach gateway.
// =====================================================
void processIncoming() {
  feedGPS();

  int packetSize = LoRa.parsePacket();
  if (!packetSize) return;

  String rx = "";
  while (LoRa.available()) rx += (char)LoRa.read();
  rx.trim();

  String key  = getField(rx, 0);
  String type = getField(rx, 1);

  // Handle gateway beacons
  if (type == "BEACON" && key == "CENSYS_GW") {
    lastBeaconMillis = millis();
    gatewayReachable = true;
    Serial.println("Beacon received from gateway");
    return;
  }

  if (!(key == "CENSYS_N1_2026" || key == "CENSYS_N2_2026" || key == "CENSYS_N3_2026" || key == "CENSYS_N4_2026")) {
    return;
  }

  // Handle ACK
  if (type == "ACK") {
    String dest = getField(rx, 2);
    if (dest.toInt() == NODE_ID) {
      lastAckTime = millis();
      lastBeaconMillis = millis();
      gatewayReachable = true;
    }
    return;
  }

  if (type != "DATA") return;

  String originStr = getField(rx, 2);
  String senderStr = getField(rx, 3);
  String seqStr    = getField(rx, 4);
  String hopsStr   = getField(rx, 5);

  int origin = originStr.toInt();
  int sender = senderStr.toInt();
  int hops   = hopsStr.toInt();

  if (origin == NODE_ID) return;
  if (sender == NODE_ID) return;

  String sig = key + "|" + originStr + "|" + seqStr;
  if (isSeen(sig)) return;
  addSeen(sig);

  // ===== SMART RELAY LOGIC =====
  // Only relay if:
  // 1. This node can reach the gateway (has recent beacon/ACK)
  // 2. The packet hasn't exceeded max hops
  // 3. ANY hop count is eligible (removed the hops < 1 restriction)
  if (!gatewayReachable) {
    Serial.println("Skipping relay — this node can't reach gateway either");
    return;
  }
  if (hops >= MAX_HOPS) {
    Serial.println("Skipping relay — max hops reached");
    return;
  }

  String tempStr   = getField(rx, 6);
  String humStr    = getField(rx, 7);
  String mq2Str    = getField(rx, 8);
  String fireStr   = getField(rx, 9);
  String latStr    = getField(rx, 10);
  String lngStr    = getField(rx, 11);
  String healthStr = getField(rx, 12);
  String pathStr   = getField(rx, 13);

  String newPath = pathStr + ">" + String(NODE_ID);
  String relayPacket =
    key + "|DATA|" + originStr + "|" + String(NODE_ID) + "|" +
    seqStr + "|" + String(hops + 1) + "|" +
    tempStr + "|" + humStr + "|" + mq2Str + "|" + fireStr + "|" +
    latStr + "|" + lngStr + "|" + healthStr + "|" + newPath;

  // Random delay based on NODE_ID to prevent relay collisions
  delay(random(200 + (NODE_ID * 150), 500 + (NODE_ID * 200)));
  sendLoRa(relayPacket);

  Serial.println("RELAYED from Node " + originStr + " (hops:" + String(hops+1) + ") path:" + newPath);
}

// =====================================================
// GPS CACHE
// =====================================================
void updateGPSCache() {
  feedGPS();
  if (gps.location.isValid() && gps.location.isUpdated()) {
    double newLat = gps.location.lat();
    double newLng = gps.location.lng();
    // Philippines bounding box sanity check
    if (newLat > 4.0 && newLat < 22.0 && newLng > 116.0 && newLng < 128.0) {
      // HDOP quality filter — only accept fixes with good accuracy
      if (gps.hdop.isValid() && gps.hdop.hdop() < 5.0) {
        cachedLat = newLat;
        cachedLng = newLng;
        hasCachedGPS = true;
        lastGPSFixMillis = millis();
      } else if (!hasCachedGPS) {
        // Accept any fix if we have nothing cached yet
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

  // ===== LORA SETTINGS — SF10 for best range+reliability with 4 nodes =====
  LoRa.setSpreadingFactor(10);
  LoRa.setSignalBandwidth(125E3);
  LoRa.setCodingRate4(8);                          // 4/8 = maximum error correction
  LoRa.setTxPower(20, PA_OUTPUT_PA_BOOST_PIN);
  LoRa.setPreambleLength(8);
  LoRa.enableCrc();
  LoRa.setGain(0);                                 // AGC auto gain

  Serial.println("LoRa: SF10, BW125k, CR4/8, TX20dBm, CRC ON");
  LoRa.receive();

  randomSeed(analogRead(35));

  bootMillis = millis();

  // ===== GATEWAY DISCOVERY PHASE =====
  // Listen for gateway beacons for up to STARTUP_LISTEN_MS
  Serial.println("Scanning for gateway beacon...");
  unsigned long scanStart = millis();
  while (millis() - scanStart < STARTUP_LISTEN_MS) {
    feedGPS();  // Keep feeding GPS during discovery
    if (checkForBeacon()) {
      Serial.println("Gateway FOUND during startup scan!");
      break;
    }
    delay(10);
  }

  if (!gatewayReachable) {
    Serial.println("Gateway not detected — will operate in mesh relay mode");
    Serial.println("Will keep listening for beacons during operation");
  }

  startupDiscoveryDone = true;

  // STAGGER: offset the first send based on NODE_ID
  lastSendTime = millis() - SEND_INTERVAL_MS + (NODE_ID * 1800UL);

  Serial.println("Node " + String(NODE_ID) + " Ready | Passkey: " + NODE_PASSKEY);
  Serial.println("Gateway: " + String(gatewayReachable ? "REACHABLE" : "NOT FOUND — mesh mode"));
}

// =====================================================
// LOOP
// =====================================================
void loop() {
  // Feed GPS continuously
  feedGPS();
  updateGPSCache();

  // Update gateway reachability based on beacon timeout
  if (gatewayReachable && (millis() - lastBeaconMillis > BEACON_TIMEOUT_MS)) {
    gatewayReachable = false;
    Serial.println("Gateway beacon timeout — switching to mesh relay mode");
  }

  // Process incoming LoRa packets (beacons, relays, ACKs)
  processIncoming();

  // Time to send?
  unsigned long sendJitter = random(0, 500);  // ±500ms jitter to avoid sync collisions
  if (millis() - lastSendTime >= (SEND_INTERVAL_MS + sendJitter)) {
    lastSendTime = millis();
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

    Serial.println("---- TX Node " + String(NODE_ID) + " Seq:" + String(seqCounter) + " ----");
    Serial.println("T:" + tempStr + " H:" + humStr + " S:" + mq2Str + " F:" + fireStr);
    Serial.println("GPS:" + gpsStatus + " Sat:" + String(satCount) +
                   " HDOP:" + (gps.hdop.isValid() ? String(gps.hdop.hdop(), 1) : "N/A"));
    Serial.println("Gateway:" + String(gatewayReachable ? "REACHABLE" : "UNREACHABLE (mesh)"));

    sendLoRa(payload);
    bool ackOk = waitForAck(seqCounter);

    if (ackOk) {
      noAckCount = 0;
      Serial.println("ACK: OK");
    } else {
      noAckCount++;
      Serial.println("ACK: FAIL (" + String(noAckCount) + " consecutive)");

      // After 3+ consecutive failures, mark gateway as potentially unreachable
      if (noAckCount >= 3 && gatewayReachable) {
        // Don't immediately switch — wait for beacon timeout
        Serial.println("WARNING: Multiple ACK failures — gateway may be unreachable");
      }

      // Retry with adaptive back-off
      if (noAckCount >= 2 && noAckCount <= 5) {
        int backoff = random(300 + (noAckCount * 200), 600 + (noAckCount * 300));
        delay(backoff);
        sendLoRa(payload);
        ackOk = waitForAck(seqCounter);
        if (ackOk) {
          noAckCount = 0;
          Serial.println("RETRY ACK: OK");
        } else {
          Serial.println("RETRY ACK: FAIL");
        }
      }
    }
  }

  // Update LED (but don't read sensors every single loop — too fast)
  static unsigned long lastLedUpdate = 0;
  if (millis() - lastLedUpdate >= 500) {
    lastLedUpdate = millis();
    float tempNow = dht.readTemperature();
    float humNow  = dht.readHumidity();
    int mq2Now    = analogRead(MQ2_PIN);
    int flameNow  = digitalRead(FLAME_PIN);
    updateLED(tempNow, humNow, mq2Now, flameNow);
  }
}
