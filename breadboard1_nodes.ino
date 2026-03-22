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
const unsigned long GPS_CACHE_MAX_AGE_MS = 300000;  // 5 minutes max cache age

// =====================================================
// TIMING
// =====================================================
const unsigned long SEND_INTERVAL_MS = 5000;
const unsigned long ACK_TIMEOUT_MS   = 1200;   // Increased for SF12 (slower air time)
const unsigned long NETWORK_OK_MS    = 20000;
const int MAX_HOPS = 6;

// =====================================================
// STATE
// =====================================================
unsigned long lastSendTime = 0;
unsigned long lastAckTime = 0;
unsigned long lastLedBlink = 0;
bool ledState = false;
unsigned long seqCounter = 0;

// =====================================================
// DUPLICATE TRACKER
// =====================================================
const int SEEN_MAX = 30;
String seenPackets[SEEN_MAX];
int seenIndex = 0;

// =====================================================
// GPS INITIALIZATION COMMANDS
// Sends PMTK commands to optimize GPS module for
// better indoor/weak-signal performance.
// =====================================================
void initGPSModule() {
  // Set update rate to 1Hz (1000ms)
  gpsSerial.println("$PMTK220,1000*1F");
  delay(100);

  // Enable all NMEA sentences that help with position
  // GGA, RMC, GSA, GSV for full satellite info
  gpsSerial.println("$PMTK314,0,1,0,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0*28");
  delay(100);

  // Enable SBAS (Satellite-Based Augmentation System)
  // Improves accuracy using geostationary satellite corrections
  gpsSerial.println("$PMTK313,1*2E");
  delay(100);

  // Enable SBAS integrity mode
  gpsSerial.println("$PMTK301,2*2E");
  delay(100);

  // Enable AIC (Active Interference Cancellation)
  // Reduces interference from nearby electronics
  gpsSerial.println("$PMTK286,1*23");
  delay(100);

  // Enable EASY (Embedded Assist System)
  // Uses predicted orbits for faster time-to-first-fix
  gpsSerial.println("$PMTK869,1,1*35");
  delay(100);

  // Set navigation mode to pedestrian/stationary
  // Better for fixed sensor nodes — more aggressive position hold
  gpsSerial.println("$PMTK886,1*25");
  delay(100);

  Serial.println("GPS: PMTK init commands sent (SBAS, AIC, EASY, pedestrian mode)");
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

String smokeLevel(int mq2) {
  if (mq2 < 0) return "Needs replacement";
  if (mq2 >= 900) return "Heavy Smoke / Fire";
  if (mq2 >= 600) return "Smoke";
  if (mq2 >= 150) return "Clean Air";
  return "Very Clean / Sensor Warming";
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
  if (mq2 < 0 || mq2 > 4095) return true;
  return false;
}

bool isFlameBad(int flameVal) {
  if (flameVal != LOW && flameVal != HIGH) return true;
  return false;
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
    // slow blink = needs maintenance
    if (millis() - lastLedBlink >= 800) {
      lastLedBlink = millis();
      ledState = !ledState;
      digitalWrite(LED_PIN, ledState);
    }
  } else if (!networkOk) {
    // fast blink = finding/connecting to LoRa
    if (millis() - lastLedBlink >= 180) {
      lastLedBlink = millis();
      ledState = !ledState;
      digitalWrite(LED_PIN, ledState);
    }
  } else {
    // stable = connected to LoRa and working
    digitalWrite(LED_PIN, HIGH);
    ledState = true;
  }
}

void sendPacket(String packet) {
  LoRa.beginPacket();
  LoRa.print(packet);
  LoRa.endPacket();
  LoRa.receive();  // Return to receive mode immediately after sending
}

bool waitForAck(unsigned long mySeq) {
  unsigned long t0 = millis();

  while (millis() - t0 < ACK_TIMEOUT_MS) {
    // Feed GPS while waiting (don't waste time)
    while (gpsSerial.available()) gps.encode(gpsSerial.read());

    int packetSize = LoRa.parsePacket();
    if (!packetSize) continue;

    String rx = "";
    while (LoRa.available()) rx += (char)LoRa.read();
    rx.trim();

    String key   = getField(rx, 0);
    String type  = getField(rx, 1);
    String dest  = getField(rx, 2);
    String seq   = getField(rx, 3);

    if (type == "ACK" && key == NODE_PASSKEY) {
      if (dest.toInt() == NODE_ID && seq.toInt() == (int)mySeq) {
        lastAckTime = millis();
        return true;
      }
    }
  }
  return false;
}

void processIncoming() {
  // Feed GPS data aggressively
  while (gpsSerial.available()) gps.encode(gpsSerial.read());

  int packetSize = LoRa.parsePacket();
  if (!packetSize) return;

  String rx = "";
  while (LoRa.available()) rx += (char)LoRa.read();
  rx.trim();

  String key     = getField(rx, 0);
  String type    = getField(rx, 1);

  // Ignore if not for this network/passkey family
  if (!(key == "CENSYS_N1_2026" || key == "CENSYS_N2_2026" || key == "CENSYS_N3_2026" || key == "CENSYS_N4_2026")) {
    return;
  }

  // ACK packet
  if (type == "ACK") {
    String dest = getField(rx, 2);
    if (dest.toInt() == NODE_ID) {
      lastAckTime = millis();
    }
    return;
  }

  // DATA packet relay
  String originStr = getField(rx, 2);
  String senderStr = getField(rx, 3);
  String seqStr    = getField(rx, 4);
  String hopsStr   = getField(rx, 5);

  int origin = originStr.toInt();
  int sender = senderStr.toInt();
  int hops   = hopsStr.toInt();

  // avoid self-loop
  if (origin == NODE_ID) return;
  if (sender == NODE_ID) return;

  String sig = key + "|" + originStr + "|" + seqStr;
  if (isSeen(sig)) return;
  addSeen(sig);

  if (hops >= MAX_HOPS) return;

  // relay packet
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
    key + "|" +
    "DATA" + "|" +
    originStr + "|" +
    String(NODE_ID) + "|" +
    seqStr + "|" +
    String(hops + 1) + "|" +
    tempStr + "|" +
    humStr + "|" +
    mq2Str + "|" +
    fireStr + "|" +
    latStr + "|" +
    lngStr + "|" +
    healthStr + "|" +
    newPath;

  delay(random(60, 180));
  sendPacket(relayPacket);

  Serial.println("--------------------------------");
  Serial.println("Relayed packet");
  Serial.print("Origin Node: "); Serial.println(originStr);
  Serial.print("From Node: ");   Serial.println(senderStr);
  Serial.print("Through Node: ");Serial.println(NODE_ID);
  Serial.print("Seq: ");         Serial.println(seqStr);
}

// =====================================================
// UPDATE GPS CACHE
// Stores last known good coordinates for indoor use
// =====================================================
void updateGPSCache() {
  // Feed all available GPS data
  while (gpsSerial.available()) {
    gps.encode(gpsSerial.read());
  }

  // If we have a valid live fix, cache it
  if (gps.location.isValid() && gps.location.isUpdated()) {
    double newLat = gps.location.lat();
    double newLng = gps.location.lng();

    // Sanity check — valid coordinates for Philippines area
    if (newLat > 4.0 && newLat < 22.0 && newLng > 116.0 && newLng < 128.0) {
      cachedLat = newLat;
      cachedLng = newLng;
      hasCachedGPS = true;
      lastGPSFixMillis = millis();
    }
  }
}

// =====================================================
// GET BEST AVAILABLE GPS COORDINATES
// Returns live fix if available, otherwise cached
// =====================================================
bool getGPSCoordinates(double &lat, double &lng, String &gpsStatus) {
  if (gps.location.isValid()) {
    lat = gps.location.lat();
    lng = gps.location.lng();
    gpsStatus = "Live";
    return true;
  }

  // Use cached coordinates if available and not too old
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

  // GPS init at 9600 baud (default for most modules)
  gpsSerial.begin(9600, SERIAL_8N1, 16, 17);
  delay(500);

  // Send GPS optimization commands
  initGPSModule();

  // LoRa init with MAXIMUM RANGE settings
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);
  if (!LoRa.begin(433E6)) {
    Serial.println("LoRa init failed!");
    while (1) {
      digitalWrite(LED_PIN, !digitalRead(LED_PIN));
      delay(120);
    }
  }

  // ===== MAXIMIZE LORA RANGE (must match Breadboard 2 gateway) =====
  LoRa.setSpreadingFactor(12);                    // SF12 = maximum range, best wall penetration
  LoRa.setSignalBandwidth(125E3);                 // 125kHz = good balance of range and throughput
  LoRa.setCodingRate4(8);                          // 4/8 = maximum error correction
  LoRa.setTxPower(20, PA_OUTPUT_PA_BOOST_PIN);    // 20dBm = maximum transmit power
  LoRa.setPreambleLength(12);                      // Longer preamble = better sync through obstacles
  LoRa.enableCrc();                                // CRC catches corrupted packets automatically
  LoRa.setGain(0);                                 // AGC auto gain = best receive sensitivity

  Serial.println("LoRa: SF12, BW125k, CR4/8, TX20dBm, Preamble12, CRC ON");

  // Put LoRa in continuous receive mode
  LoRa.receive();

  randomSeed(analogRead(35));

  Serial.println("Breadboard 1 Node Ready (LoRa + GPS Optimized)");
  Serial.print("Node ID: "); Serial.println(NODE_ID);
  Serial.print("Passkey: "); Serial.println(NODE_PASSKEY);
}

// =====================================================
// LOOP
// =====================================================
void loop() {
  // Feed GPS data aggressively — process all available bytes every loop
  while (gpsSerial.available()) gps.encode(gpsSerial.read());

  // Update GPS cache with any new fix
  updateGPSCache();

  // Process incoming LoRa packets (relay for mesh)
  processIncoming();

  if (millis() - lastSendTime >= SEND_INTERVAL_MS) {
    lastSendTime = millis();
    seqCounter++;

    float temp = dht.readTemperature();
    float hum  = dht.readHumidity();
    int mq2Raw = analogRead(MQ2_PIN);
    int flameVal = digitalRead(FLAME_PIN);

    // Get GPS coordinates (live or cached)
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
      NODE_PASSKEY + "|" +
      "DATA" + "|" +
      String(NODE_ID) + "|" +
      String(NODE_ID) + "|" +
      String(seqCounter) + "|" +
      "0" + "|" +
      tempStr + "|" +
      humStr + "|" +
      mq2Str + "|" +
      fireStr + "|" +
      String(lat, 6) + "|" +
      String(lng, 6) + "|" +
      healthStr + "|" +
      String(NODE_ID);

    String sig = NODE_PASSKEY + "|" + String(NODE_ID) + "|" + String(seqCounter);
    addSeen(sig);

    Serial.println("--------------------------------");
    Serial.print("Node: "); Serial.println(NODE_ID);
    Serial.print("Temp: "); Serial.println(tempStr);
    Serial.print("Humid: "); Serial.println(humStr);
    Serial.print("Smoke: "); Serial.println(mq2Str);
    Serial.print("Fire: "); Serial.println(fireStr);
    Serial.print("GPS: "); Serial.print(gpsStatus);
    Serial.print(" | Lat: "); Serial.print(lat, 6);
    Serial.print(" | Lng: "); Serial.println(lng, 6);
    if (hasCachedGPS) {
      Serial.print("Cached GPS Age: "); Serial.print((millis() - lastGPSFixMillis) / 1000); Serial.println("s");
    }
    Serial.print("Satellites: "); Serial.println(gps.satellites.value());
    Serial.print("Health: "); Serial.println(healthStr);
    Serial.print("Seq: "); Serial.println(seqCounter);

    sendPacket(payload);
    bool ackOk = waitForAck(seqCounter);

    Serial.print("ACK: ");
    Serial.println(ackOk ? "OK" : "NO ACK");
  }

  float tempNow = dht.readTemperature();
  float humNow  = dht.readHumidity();
  int mq2Now    = analogRead(MQ2_PIN);
  int flameNow  = digitalRead(FLAME_PIN);

  updateLED(tempNow, humNow, mq2Now, flameNow);
}
