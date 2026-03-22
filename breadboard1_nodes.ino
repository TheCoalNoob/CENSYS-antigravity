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
const unsigned long SEND_INTERVAL_MS = 5000;   // 5 seconds between sends
const unsigned long ACK_TIMEOUT_MS   = 800;    // ACK wait time
const unsigned long NETWORK_OK_MS    = 20000;
const int MAX_HOPS = 3;

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
    if (millis() - lastLedBlink >= 800) {
      lastLedBlink = millis();
      ledState = !ledState;
      digitalWrite(LED_PIN, ledState);
    }
  } else if (!networkOk) {
    if (millis() - lastLedBlink >= 180) {
      lastLedBlink = millis();
      ledState = !ledState;
      digitalWrite(LED_PIN, ledState);
    }
  } else {
    digitalWrite(LED_PIN, HIGH);
    ledState = true;
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

bool waitForAck(unsigned long mySeq) {
  unsigned long t0 = millis();
  while (millis() - t0 < ACK_TIMEOUT_MS) {
    while (gpsSerial.available()) gps.encode(gpsSerial.read());
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
    String dest = getField(rx, 2);
    String seq  = getField(rx, 3);
    if (type == "ACK" && key == NODE_PASSKEY) {
      if (dest.toInt() == NODE_ID && seq.toInt() == (int)mySeq) {
        lastAckTime = millis();
        return true;
      }
    }
  }
  return false;
}

// =====================================================
// PROCESS INCOMING (relay for mesh)
// Only relay packets that have already been relayed
// (hops >= 1), meaning they came from a node that
// can't reach the gateway directly.
// =====================================================
void processIncoming() {
  while (gpsSerial.available()) gps.encode(gpsSerial.read());

  int packetSize = LoRa.parsePacket();
  if (!packetSize) return;

  String rx = "";
  while (LoRa.available()) rx += (char)LoRa.read();
  rx.trim();

  String key  = getField(rx, 0);
  String type = getField(rx, 1);

  if (!(key == "CENSYS_N1_2026" || key == "CENSYS_N2_2026" || key == "CENSYS_N3_2026" || key == "CENSYS_N4_2026")) {
    return;
  }

  // Handle ACK
  if (type == "ACK") {
    String dest = getField(rx, 2);
    if (dest.toInt() == NODE_ID) {
      lastAckTime = millis();
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

  // ONLY relay packets that are already being relayed (hops >= 1)
  // If hops == 0, the node sent directly and the gateway probably got it
  // This prevents relay storms when all nodes can reach the gateway
  if (hops < 1) return;
  if (hops >= MAX_HOPS) return;

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

  // Long delay based on NODE_ID to prevent relay collisions
  delay(random(300 + (NODE_ID * 200), 600 + (NODE_ID * 300)));
  sendLoRa(relayPacket);

  Serial.println("Relayed from Node " + originStr + " (hops:" + String(hops+1) + ")");
}

// =====================================================
// GPS CACHE
// =====================================================
void updateGPSCache() {
  while (gpsSerial.available()) gps.encode(gpsSerial.read());
  if (gps.location.isValid() && gps.location.isUpdated()) {
    double newLat = gps.location.lat();
    double newLng = gps.location.lng();
    if (newLat > 4.0 && newLat < 22.0 && newLng > 116.0 && newLng < 128.0) {
      cachedLat = newLat;
      cachedLng = newLng;
      hasCachedGPS = true;
      lastGPSFixMillis = millis();
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
  // SF10 at 433MHz + 20dBm = excellent wall penetration
  // Air time ~0.5s per packet — allows 4 nodes to share the channel
  LoRa.setSpreadingFactor(10);
  LoRa.setSignalBandwidth(125E3);
  LoRa.setCodingRate4(8);                          // 4/8 = maximum error correction for reliability
  LoRa.setTxPower(20, PA_OUTPUT_PA_BOOST_PIN);
  LoRa.setPreambleLength(8);
  LoRa.enableCrc();
  LoRa.setGain(0);                                 // AGC auto gain

  Serial.println("LoRa: SF10, BW125k, CR4/8, TX20dBm, CRC ON");
  LoRa.receive();

  randomSeed(analogRead(35));

  // STAGGER: offset the first send based on NODE_ID
  // Node 1 starts at 1s, Node 2 at 2.2s, Node 3 at 3.4s, Node 4 at 4.6s
  // This naturally separates transmissions without complex modulo math
  lastSendTime = millis() - SEND_INTERVAL_MS + (NODE_ID * 1200UL);

  Serial.println("Node " + String(NODE_ID) + " Ready | Passkey: " + NODE_PASSKEY);
}

// =====================================================
// LOOP
// =====================================================
void loop() {
  // Feed GPS continuously
  while (gpsSerial.available()) gps.encode(gpsSerial.read());
  updateGPSCache();

  // Process incoming LoRa packets
  processIncoming();

  // Time to send?
  if (millis() - lastSendTime >= SEND_INTERVAL_MS) {
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
    Serial.println("GPS:" + gpsStatus + " Sat:" + String(gps.satellites.value()));

    sendLoRa(payload);
    bool ackOk = waitForAck(seqCounter);

    if (ackOk) {
      noAckCount = 0;
      Serial.println("ACK: OK");
    } else {
      noAckCount++;
      Serial.println("ACK: FAIL (" + String(noAckCount) + " consecutive)");

      // Retry once after a short delay if we keep failing
      if (noAckCount >= 2 && noAckCount <= 4) {
        delay(random(200, 500));
        sendLoRa(payload);
        ackOk = waitForAck(seqCounter);
        if (ackOk) {
          noAckCount = 0;
          Serial.println("RETRY ACK: OK");
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
