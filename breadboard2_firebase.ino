#include <SPI.h>
#include <LoRa.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

// =====================================================
// LORA PINS
// =====================================================
#define LORA_SS   5
#define LORA_RST  14
#define LORA_DIO0 26

// =====================================================
// STATUS LED
// =====================================================
#define LED_PIN 25

// =====================================================
// WIFI ACCESS POINT (Local gateway dashboard)
// =====================================================
const char* AP_SSID = "CENSYS-Gateway";
const char* AP_PASS = "censys123";

// =====================================================
// WIFI STATION (Internet for Firebase)
// Replace with your actual WiFi network credentials
// =====================================================
const char* STA_SSID = "IKYK";
const char* STA_PASS = "444everydayOK*";

// =====================================================
// FIREBASE CONFIG
// Replace with your Firebase Realtime Database URL and secret
// =====================================================
#define FIREBASE_HOST "https://censys-antigravity-default-rtdb.asia-southeast1.firebasedatabase.app"
#define FIREBASE_AUTH "gXuLrZRM6GoMQKKwuHzWNDyp2Sd3x4CefsNG5kFc"  // Get from Firebase Console > Project Settings > Service accounts > Database secrets

// =====================================================
// Push interval — don't flood Firebase
// =====================================================
const unsigned long FIREBASE_PUSH_INTERVAL_MS = 2000;  // Push ONE node every 2 seconds (round-robin)
const unsigned long FIREBASE_STATUS_PUSH_MS = 15000;    // Periodic status refresh
unsigned long lastFirebasePush = 0;
unsigned long lastStatusPush = 0;
int firebasePushIndex = 0;         // Round-robin: 1-4 = nodes, 5 = gateway, 6+ = unreg
bool firebaseDataDirty = false;
bool firebaseUnregDirty = false;

// =====================================================
// BEACON TIMING
// =====================================================
const unsigned long BEACON_INTERVAL_MS = 15000;  // Broadcast beacon every 15 seconds (avoid TDMA slot interference)
unsigned long lastBeaconSent = 0;

WebServer server(80);

// =====================================================
// REGISTERED NODE PASSKEYS
// =====================================================
const int REGISTERED_NODES = 4;
String NODE_KEYS[REGISTERED_NODES] = {
  "CENSYS_N1_2026",
  "CENSYS_N2_2026",
  "CENSYS_N3_2026",
  "CENSYS_N4_2026"
};

// =====================================================
// NODE-TO-BARANGAY MAPPING
// Change the barangay name for each node as needed.
// Index 0 = unused, index 1..4 = node 1..4
// =====================================================
String NODE_BARANGAY[5] = {
  "",           // unused (index 0)
  "kalunasan",  // Node 1
  "kalunasan",  // Node 2
  "kalunasan",  // Node 3
  "kalunasan"   // Node 4
};

// =====================================================
// NODE DATA STRUCTURE
// =====================================================
struct NodeData {
  bool online;
  String passkey;
  int nodeId;
  int lastSender;
  unsigned long seq;
  int hops;
  String temp;
  String humid;
  String smoke;
  String fire;
  String lat;
  String lng;
  String health;
  String path;
  String category;
  int rssi;
  unsigned long lastSeenMillis;
};

NodeData nodes[5];   // use index 1..4

// =====================================================
// GPS PERSISTENCE — saved separately so they survive
// no-fix / 0.0 / empty updates from nodes
// =====================================================
String savedLat[5] = {"", "", "", "", ""};
String savedLng[5] = {"", "", "", "", ""};

bool isValidGPS(String lat, String lng) {
  lat.trim(); lng.trim();
  if (lat.length() == 0 || lng.length() == 0) return false;
  if (lat == "-" || lng == "-") return false;
  if (lat == "0" || lat == "0.0" || lat == "0.00" || lat == "0.000000") return false;
  if (lng == "0" || lng == "0.0" || lng == "0.00" || lng == "0.000000") return false;
  float fLat = lat.toFloat();
  float fLng = lng.toFloat();
  if (fLat == 0.0 || fLng == 0.0) return false;
  return true;
}

const unsigned long NODE_TIMEOUT_MS = 30000;  // 30 seconds (matches TDMA cycle 12s * 2.5)

// =====================================================
// TEMPORAL STABILITY TRACKER (prevents false fire alarms)
// =====================================================
const int STABILITY_WINDOW = 3;
String categoryHistory[5][3];  // [nodeId][history slot]
int categoryHistoryIdx[5] = {0, 0, 0, 0, 0};

void addCategoryHistory(int nodeId, String cat) {
  if (nodeId < 1 || nodeId > 4) return;
  categoryHistory[nodeId][categoryHistoryIdx[nodeId]] = cat;
  categoryHistoryIdx[nodeId] = (categoryHistoryIdx[nodeId] + 1) % STABILITY_WINDOW;
}

// Returns true if Fire should be accepted (2 of last 3 readings are Fire or Warning)
bool isFireStable(int nodeId) {
  if (nodeId < 1 || nodeId > 4) return false;
  int votes = 0;
  for (int i = 0; i < STABILITY_WINDOW; i++) {
    if (categoryHistory[nodeId][i] == "Fire" || categoryHistory[nodeId][i] == "Warning") {
      votes++;
    }
  }
  return votes >= 2;
}

// =====================================================
// UNREGISTERED NODE DATA (up to 8 tracked)
// =====================================================
const int MAX_UNREG = 8;
struct UnregNodeData {
  bool active;
  String passkey;
  int nodeId;
  String temp;
  String humid;
  String smoke;
  String fire;
  String health;
  int rssi;
  int hops;
  String path;
  unsigned long lastSeenMillis;
};

UnregNodeData unregNodes[MAX_UNREG];
int unregCount = 0;

int findOrAddUnreg(String key, int nodeId) {
  // Find existing
  for (int i = 0; i < MAX_UNREG; i++) {
    if (unregNodes[i].active && unregNodes[i].passkey == key && unregNodes[i].nodeId == nodeId) {
      return i;
    }
  }
  // Find empty slot
  for (int i = 0; i < MAX_UNREG; i++) {
    if (!unregNodes[i].active) {
      unregNodes[i].active = true;
      return i;
    }
  }
  // Evict oldest
  int oldest = 0;
  unsigned long oldestTime = unregNodes[0].lastSeenMillis;
  for (int i = 1; i < MAX_UNREG; i++) {
    if (unregNodes[i].lastSeenMillis < oldestTime) {
      oldestTime = unregNodes[i].lastSeenMillis;
      oldest = i;
    }
  }
  return oldest;
}

// =====================================================
// DUPLICATE TRACKER
// =====================================================
const int SEEN_MAX = 60;
String seenPackets[SEEN_MAX];
int seenIndex = 0;

// =====================================================
// LED STATE
// =====================================================
unsigned long lastLedBlink = 0;
bool ledState = false;

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

bool isRegisteredKey(String key) {
  for (int i = 0; i < REGISTERED_NODES; i++) {
    if (NODE_KEYS[i] == key) return true;
  }
  return false;
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

bool isNodeOnline(int nodeId) {
  if (nodeId < 1 || nodeId > 4) return false;
  if (nodes[nodeId].lastSeenMillis == 0) return false;
  return (millis() - nodes[nodeId].lastSeenMillis) < NODE_TIMEOUT_MS;
}

bool allNodesWorking() {
  for (int i = 1; i <= 4; i++) {
    if (!isNodeOnline(i)) return false;
  }
  return true;
}

bool anyNodeOnline() {
  for (int i = 1; i <= 4; i++) {
    if (isNodeOnline(i)) return true;
  }
  return false;
}

void updateGatewayLED() {
  if (anyNodeOnline()) {
    // Solid = at least one node is connected and sending data
    digitalWrite(LED_PIN, HIGH);
    ledState = true;
  } else {
    // Fast blink = no nodes connected
    if (millis() - lastLedBlink >= 180) {
      lastLedBlink = millis();
      ledState = !ledState;
      digitalWrite(LED_PIN, ledState);
    }
  }
}

void sendAck(String key, int originNode, String seqStr) {
  String ack = key + "|ACK|" + String(originNode) + "|" + seqStr;

  delay(50);                 // 50ms delay to let channel settle after node TX
  LoRa.idle();
  LoRa.beginPacket();
  LoRa.print(ack);
  LoRa.endPacket();          // BLOCKING — ensure ACK is fully sent
  delay(20);
  LoRa.receive();

  Serial.println("ACK -> Node " + String(originNode));
}

// =====================================================
// GATEWAY BEACON BROADCAST
// Short packet so nodes can discover the gateway
// =====================================================
void sendBeacon() {
  String beacon = "CENSYS_GW|BEACON|" + String(millis());
  LoRa.idle();
  LoRa.beginPacket();
  LoRa.print(beacon);
  LoRa.endPacket(true);      // Non-blocking
  delay(60);
  LoRa.receive();
  // Only log every 3rd beacon to reduce serial spam
  static int beaconCount = 0;
  beaconCount++;
  if (beaconCount % 3 == 0) {
    Serial.println("BEACON sent (" + String(beaconCount) + ")");
  }
}

String nodeStatusText(int nodeId) {
  return isNodeOnline(nodeId) ? "Online" : "Offline";
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

// =====================================================
// NOISE VALIDATION — detect garbage LoRa data
// =====================================================
bool isValidFloat(String s) {
  if (s.length() == 0) return false;
  if (s == "Needs replacement") return true;
  bool hasDot = false;
  int start = 0;
  if (s.charAt(0) == '-') start = 1;
  if (start >= (int)s.length()) return false;
  for (int i = start; i < (int)s.length(); i++) {
    char c = s.charAt(i);
    if (c == '.') {
      if (hasDot) return false;
      hasDot = true;
    } else if (c < '0' || c > '9') {
      return false;
    }
  }
  return true;
}

bool isValidInt(String s) {
  if (s.length() == 0) return false;
  if (s == "Needs replacement") return true;
  int start = 0;
  if (s.charAt(0) == '-') start = 1;
  if (start >= (int)s.length()) return false;
  for (int i = start; i < (int)s.length(); i++) {
    if (s.charAt(i) < '0' || s.charAt(i) > '9') return false;
  }
  return true;
}

bool hasNonPrintable(String s) {
  for (int i = 0; i < (int)s.length(); i++) {
    char c = s.charAt(i);
    if (c < 32 && c != '\t') return true;
    if ((unsigned char)c > 126) return true;
  }
  return false;
}

int countFields(String data) {
  int count = 1;
  for (int i = 0; i < (int)data.length(); i++) {
    if (data.charAt(i) == '|') count++;
  }
  return count;
}

// Returns true if the packet looks like noise/garbage
bool isNoisePacket(String rx, String tempStr, String humStr, String smokeStr, String fireStr, String originStr, String latStr, String lngStr) {
  // Check for non-printable chars in the whole packet
  if (hasNonPrintable(rx)) {
    Serial.println("NOISE: Non-printable characters detected");
    return true;
  }

  // Must have exactly 14 fields
  if (countFields(rx) != 14) {
    Serial.println("NOISE: Wrong field count (" + String(countFields(rx)) + " instead of 14)");
    return true;
  }

  // Validate origin node ID
  if (!isValidInt(originStr) || originStr.toInt() < 1) {
    Serial.println("NOISE: Invalid origin node ID: " + originStr);
    return true;
  }

  // Validate temp
  if (!isValidFloat(tempStr)) {
    Serial.println("NOISE: Invalid temp value: " + tempStr);
    return true;
  }

  // Validate humidity
  if (!isValidFloat(humStr)) {
    Serial.println("NOISE: Invalid humidity value: " + humStr);
    return true;
  }

  // Validate smoke (MQ2 reading = integer)
  if (!isValidInt(smokeStr)) {
    Serial.println("NOISE: Invalid smoke value: " + smokeStr);
    return true;
  }

  // Validate fire sensor
  if (fireStr != "Flame" && fireStr != "None" && fireStr != "Needs replacement") {
    Serial.println("NOISE: Invalid fire value: " + fireStr);
    return true;
  }

  // Validate lat/lng (allow 0.0 for no GPS fix)
  if (!isValidFloat(latStr)) {
    Serial.println("NOISE: Invalid latitude: " + latStr);
    return true;
  }
  if (!isValidFloat(lngStr)) {
    Serial.println("NOISE: Invalid longitude: " + lngStr);
    return true;
  }

  return false;  // Passed all checks — not noise
}

// =====================================================
// CATEGORY CLASSIFICATION
// =====================================================
String classifyCategory(String tempStr, String humStr, String smokeStr, String fireStr, String healthStr) {
  if (healthStr.indexOf("Needs replacement") >= 0) {
    return "Warning";
  }

  float temp = tempStr.toFloat();
  float hum  = humStr.toFloat();
  int smoke  = smokeStr.toInt();
  bool flame = (fireStr == "Flame");

  int fireVotes = 0;
  int warningVotes = 0;

  if (!isReplacementValue(tempStr)) {
    if (temp >= 58.0) fireVotes++;
    else if (temp >= 39.0) warningVotes++;
  }

  if (!isReplacementValue(smokeStr)) {
    if (smoke >= 850) fireVotes++;
    else if (smoke >= 450) warningVotes++;
  }

  if (!isReplacementValue(fireStr) && flame) {
    fireVotes++;
  }

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

int countCategory(String wanted) {
  int c = 0;
  for (int i = 1; i <= 4; i++) {
    if (isNodeOnline(i) && nodes[i].category == wanted) c++;
  }
  return c;
}

// =====================================================
// FIREBASE PUSH
// =====================================================
void pushNodeToFirebase(int nodeId) {
  if (WiFi.status() != WL_CONNECTED) return;
  if (nodeId < 1 || nodeId > 4) return;

  // SKIP nodes that have NEVER sent data — don't push blank data
  if (nodes[nodeId].lastSeenMillis == 0) {
    return;
  }

  HTTPClient http;
  WiFiClientSecure client;
  client.setInsecure();

  String url = String(FIREBASE_HOST) + "/nodes/node" + String(nodeId) + ".json?auth=" + String(FIREBASE_AUTH);

  unsigned long ageSec = (millis() - nodes[nodeId].lastSeenMillis) / 1000;
  bool online = isNodeOnline(nodeId);

  if (online) {
    // ONLINE: Full PUT with all sensor data + fresh timestamp
    String json = "{";
    json += "\"online\":true,";
    json += "\"node\":" + String(nodeId) + ",";
    json += "\"barangay\":\"" + NODE_BARANGAY[nodeId] + "\",";
    json += "\"last_sender\":" + String(nodes[nodeId].lastSender) + ",";
    json += "\"seq\":" + String(nodes[nodeId].seq) + ",";
    json += "\"hops\":" + String(nodes[nodeId].hops) + ",";
    json += "\"temp\":\"" + esc(nodes[nodeId].temp) + "\",";
    json += "\"humid\":\"" + esc(nodes[nodeId].humid) + "\",";
    json += "\"smoke\":\"" + esc(nodes[nodeId].smoke) + "\",";
    json += "\"fire\":\"" + esc(nodes[nodeId].fire) + "\",";
    json += "\"lat\":\"" + esc(nodes[nodeId].lat) + "\",";
    json += "\"lng\":\"" + esc(nodes[nodeId].lng) + "\",";
    json += "\"health\":\"" + esc(nodes[nodeId].health) + "\",";
    json += "\"path\":\"" + esc(nodes[nodeId].path) + "\",";
    json += "\"category\":\"" + esc(nodes[nodeId].category) + "\",";
    json += "\"rssi\":" + String(nodes[nodeId].rssi) + ",";
    json += "\"last_seen_sec\":" + String(ageSec) + ",";
    json += "\"passkey\":\"" + esc(nodes[nodeId].passkey) + "\",";
    json += "\"timestamp\":{\".sv\":\"timestamp\"}";
    json += "}";

    http.begin(client, url);
    http.addHeader("Content-Type", "application/json");
    int httpCode = http.PUT(json);

    if (httpCode > 0) {
      Serial.print("Firebase node" + String(nodeId) + " push: ");
      Serial.println(httpCode);
    } else {
      Serial.print("Firebase node" + String(nodeId) + " error: ");
      Serial.println(http.errorToString(httpCode));
    }
  } else {
    // OFFLINE: PATCH only status fields — preserve last sensor data in Firebase
    String json = "{";
    json += "\"online\":false,";
    json += "\"last_seen_sec\":" + String(ageSec);
    json += "}";

    http.begin(client, url);
    http.addHeader("Content-Type", "application/json");
    int httpCode = http.PATCH(json);

    if (httpCode > 0) {
      Serial.print("Firebase node" + String(nodeId) + " offline PATCH: ");
      Serial.println(httpCode);
    } else {
      Serial.print("Firebase node" + String(nodeId) + " offline error: ");
      Serial.println(http.errorToString(httpCode));
    }
  }

  http.end();
}

void pushGatewayToFirebase() {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  WiFiClientSecure client;
  client.setInsecure();

  String url = String(FIREBASE_HOST) + "/gateway.json?auth=" + String(FIREBASE_AUTH);

  String json = "{";
  json += "\"ip\":\"" + WiFi.softAPIP().toString() + "\",";
  json += "\"ssid\":\"" + String(AP_SSID) + "\",";
  json += "\"all_nodes_working\":" + String(allNodesWorking() ? "true" : "false") + ",";
  json += "\"normal_count\":" + String(countCategory("Normal")) + ",";
  json += "\"warning_count\":" + String(countCategory("Warning")) + ",";
  json += "\"fire_count\":" + String(countCategory("Fire")) + ",";
  json += "\"online_count\":" + String([](){ int c=0; for(int i=1;i<=4;i++) if(isNodeOnline(i)) c++; return c; }()) + ",";
  json += "\"timestamp\":{\".sv\":\"timestamp\"}";
  json += "}";

  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");
  int httpCode = http.PUT(json);

  if (httpCode > 0) {
    Serial.print("Firebase gateway push: ");
    Serial.println(httpCode);
  } else {
    Serial.print("Firebase gateway error: ");
    Serial.println(http.errorToString(httpCode));
  }

  http.end();
}

void pushUnregToFirebase() {
  if (WiFi.status() != WL_CONNECTED) return;

  for (int i = 0; i < MAX_UNREG; i++) {
    if (!unregNodes[i].active) continue;
    // Only push if seen within last 60 seconds
    if ((millis() - unregNodes[i].lastSeenMillis) > 60000) continue;

    HTTPClient http;
    WiFiClientSecure client;
    client.setInsecure();

    String nodeKey = "unreg_" + String(i) + "_n" + String(unregNodes[i].nodeId);
    String url = String(FIREBASE_HOST) + "/unregistered_nodes/" + nodeKey + ".json?auth=" + String(FIREBASE_AUTH);

    unsigned long ageSec = (millis() - unregNodes[i].lastSeenMillis) / 1000;

    String json = "{";
    json += "\"active\":true,";
    json += "\"node\":" + String(unregNodes[i].nodeId) + ",";
    json += "\"passkey\":\"" + esc(unregNodes[i].passkey) + "\",";
    json += "\"temp\":\"" + esc(unregNodes[i].temp) + "\",";
    json += "\"humid\":\"" + esc(unregNodes[i].humid) + "\",";
    json += "\"smoke\":\"" + esc(unregNodes[i].smoke) + "\",";
    json += "\"fire\":\"" + esc(unregNodes[i].fire) + "\",";
    json += "\"health\":\"" + esc(unregNodes[i].health) + "\",";
    json += "\"rssi\":" + String(unregNodes[i].rssi) + ",";
    json += "\"hops\":" + String(unregNodes[i].hops) + ",";
    json += "\"path\":\"" + esc(unregNodes[i].path) + "\",";
    json += "\"last_seen_sec\":" + String(ageSec) + ",";
    json += "\"timestamp\":{\".sv\":\"timestamp\"}";
    json += "}";

    http.begin(client, url);
    http.addHeader("Content-Type", "application/json");
    int httpCode = http.PUT(json);

    if (httpCode > 0) {
      Serial.print("Firebase unreg " + nodeKey + " push: ");
      Serial.println(httpCode);
    } else {
      Serial.print("Firebase unreg " + nodeKey + " error: ");
      Serial.println(http.errorToString(httpCode));
    }

    http.end();
  }
}

// Push ONE item to Firebase (called repeatedly, round-robin)
// This prevents blocking LoRa for 5-10 seconds!
void pushNextToFirebase() {
  if (WiFi.status() != WL_CONNECTED) return;

  firebasePushIndex++;

  if (firebasePushIndex >= 1 && firebasePushIndex <= 4) {
    // Push one node
    pushNodeToFirebase(firebasePushIndex);
  } else if (firebasePushIndex == 5) {
    // Push gateway status
    pushGatewayToFirebase();
  } else if (firebasePushIndex == 6 && firebaseUnregDirty) {
    // Push unregistered nodes
    pushUnregToFirebase();
    firebaseUnregDirty = false;
  } else {
    // Reset cycle
    firebasePushIndex = 0;
    firebaseDataDirty = false;
  }
}

// =====================================================
// LOCAL WEB JSON
// =====================================================
String buildJson() {
  String json = "{";

  json += "\"gateway\":{";
  json += "\"ip\":\"" + WiFi.softAPIP().toString() + "\",";
  json += "\"ssid\":\"" + String(AP_SSID) + "\",";
  json += "\"all_nodes_working\":";
  json += (allNodesWorking() ? "true" : "false");
  json += ",";
  json += "\"normal_count\":" + String(countCategory("Normal")) + ",";
  json += "\"warning_count\":" + String(countCategory("Warning")) + ",";
  json += "\"fire_count\":" + String(countCategory("Fire"));
  json += "},";

  json += "\"nodes\":[";
  for (int i = 1; i <= 4; i++) {
    if (i > 1) json += ",";

    unsigned long ageSec = 0;
    if (nodes[i].lastSeenMillis > 0) {
      ageSec = (millis() - nodes[i].lastSeenMillis) / 1000;
    }

    json += "{";
    json += "\"node\":" + String(i) + ",";
    json += "\"online\":";
    json += (isNodeOnline(i) ? "true" : "false");
    json += ",";
    json += "\"status\":\"" + nodeStatusText(i) + "\",";
    json += "\"passkey\":\"" + esc(nodes[i].passkey) + "\",";
    json += "\"last_sender\":" + String(nodes[i].lastSender) + ",";
    json += "\"seq\":" + String(nodes[i].seq) + ",";
    json += "\"hops\":" + String(nodes[i].hops) + ",";
    json += "\"temp\":\"" + esc(nodes[i].temp) + "\",";
    json += "\"humid\":\"" + esc(nodes[i].humid) + "\",";
    json += "\"smoke\":\"" + esc(nodes[i].smoke) + "\",";
    json += "\"fire\":\"" + esc(nodes[i].fire) + "\",";
    json += "\"lat\":\"" + esc(nodes[i].lat) + "\",";
    json += "\"lng\":\"" + esc(nodes[i].lng) + "\",";
    json += "\"health\":\"" + esc(nodes[i].health) + "\",";
    json += "\"path\":\"" + esc(nodes[i].path) + "\",";
    json += "\"category\":\"" + esc(nodes[i].category) + "\",";
    json += "\"rssi\":" + String(nodes[i].rssi) + ",";
    json += "\"last_seen_sec\":" + String(ageSec);
    json += "}";
  }
  json += "]";

  json += "}";

  return json;
}

// =====================================================
// WEB HANDLERS (same local dashboard as before)
// =====================================================
void handleRoot() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>CENSYS Gateway Monitor</title>
<style>
  :root{
    --bg:#f4f7fb;--card:#ffffff;--line:#d9e2ec;--text:#1f2937;
    --muted:#6b7280;--ok:#15803d;--off:#b91c1c;--warn:#b45309;
    --fire:#b91c1c;--accent:#1d4ed8;--accent-soft:#dbeafe;
    --shadow:0 10px 30px rgba(0,0,0,.06);--radius:18px;
  }
  *{box-sizing:border-box}
  body{margin:0;font-family:Arial,Helvetica,sans-serif;background:linear-gradient(180deg,#eef4fb 0%,#f8fbff 100%);color:var(--text);}
  .wrap{width:min(1450px,95%);margin:24px auto;}
  .top{background:var(--card);border:1px solid var(--line);border-radius:var(--radius);box-shadow:var(--shadow);padding:20px;margin-bottom:18px;}
  .title{display:flex;flex-wrap:wrap;justify-content:space-between;gap:14px;align-items:center;}
  h1{margin:0;font-size:clamp(24px,3vw,36px);letter-spacing:.3px;}
  .sub{color:var(--muted);margin-top:6px;font-size:14px;}
  .badge{display:inline-flex;align-items:center;gap:8px;padding:10px 14px;border-radius:999px;font-size:14px;font-weight:700;background:var(--accent-soft);color:var(--accent);}
  .dot{width:10px;height:10px;border-radius:50%;background:currentColor;}
  .meta{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:12px;margin-top:18px;}
  .mini{background:#f8fbff;border:1px solid var(--line);border-radius:14px;padding:14px;}
  .mini .k{color:var(--muted);font-size:12px;text-transform:uppercase;letter-spacing:.8px;}
  .mini .v{margin-top:6px;font-weight:700;font-size:18px;}
  .grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(320px,1fr));gap:18px;}
  .card{background:var(--card);border:1px solid var(--line);border-radius:var(--radius);box-shadow:var(--shadow);padding:18px;}
  .cardhead{display:flex;justify-content:space-between;align-items:center;gap:10px;margin-bottom:14px;flex-wrap:wrap;}
  .head-left{display:flex;align-items:center;gap:10px;flex-wrap:wrap;}
  .node-title{font-size:22px;font-weight:700;}
  .status{padding:8px 12px;border-radius:999px;font-weight:700;font-size:13px;}
  .online{background:#dcfce7;color:var(--ok);}
  .offline{background:#fee2e2;color:var(--off);}
  .cat{padding:8px 12px;border-radius:999px;font-weight:700;font-size:13px;}
  .cat-normal{background:#dcfce7;color:var(--ok);}
  .cat-warning{background:#fef3c7;color:var(--warn);}
  .cat-fire{background:#fee2e2;color:var(--fire);}
  table{width:100%;border-collapse:collapse;table-layout:fixed;}
  td{padding:10px 0;border-bottom:1px solid #eef2f7;vertical-align:top;word-wrap:break-word;font-size:14px;}
  td:first-child{width:38%;color:var(--muted);font-weight:600;padding-right:12px;}
  .foot{margin-top:18px;color:var(--muted);font-size:13px;text-align:center;}
  .good{color:var(--ok);font-weight:700}
  .bad{color:var(--off);font-weight:700}
  .warn{color:var(--warn);font-weight:700}
  @media(max-width:640px){.wrap{width:min(100%,96%)}.top,.card{padding:15px}td:first-child{width:42%}}
</style>
</head>
<body>
  <div class="wrap">
    <section class="top">
      <div class="title">
        <div>
          <h1>CENSYS Gateway Local Monitor</h1>
          <div class="sub">Local diagnostics &mdash; LoRa connectivity, sensor values, fire category.</div>
        </div>
        <div id="networkBadge" class="badge"><span class="dot"></span><span>Loading...</span></div>
      </div>
      <div class="meta">
        <div class="mini"><div class="k">Gateway IP</div><div class="v" id="gatewayIp">-</div></div>
        <div class="mini"><div class="k">Wi-Fi SSID</div><div class="v" id="gatewaySsid">-</div></div>
        <div class="mini"><div class="k">Normal</div><div class="v" id="normalCount">0</div></div>
        <div class="mini"><div class="k">Warning</div><div class="v" id="warningCount">0</div></div>
        <div class="mini"><div class="k">Fire</div><div class="v" id="fireCount">0</div></div>
      </div>
    </section>
    <section class="grid" id="nodeGrid"></section>
    <div class="foot">CENSYS Technologies Inc. &mdash; Local gateway diagnostics</div>
  </div>
<script>
function field(l,v){return `<tr><td>${l}</td><td>${v??'-'}</td></tr>`;}
function sc(o){return o?'online':'offline';}
function cc(c){if(c==='Fire')return 'cat-fire';if(c==='Warning')return 'cat-warning';return 'cat-normal';}
function s(v){if(v===undefined||v===null||v==='')return '-';return String(v);}
function buildCard(n){
  const hc=String(n.health).includes('Needs replacement')?'bad':String(n.health).includes('No Fix')?'warn':'good';
  return `<article class="card"><div class="cardhead"><div class="head-left"><div class="node-title">Node ${n.node}</div><div class="status ${sc(n.online)}">${n.status}</div></div><div class="cat ${cc(n.category)}">${s(n.category)}</div></div><table>${field('Last Sender',s(n.last_sender))}${field('Sequence',s(n.seq))}${field('Hops',s(n.hops))}${field('Temperature',s(n.temp))}${field('Humidity',s(n.humid))}${field('Smoke',s(n.smoke))}${field('Fire Sensor',s(n.fire))}${field('Latitude',s(n.lat))}${field('Longitude',s(n.lng))}${field('Health',`<span class="${hc}">${s(n.health)}</span>`)}${field('Category',`<strong>${s(n.category)}</strong>`)}${field('Path',s(n.path))}${field('RSSI',s(n.rssi))}${field('Last Seen',s(n.last_seen_sec)+' sec ago')}</table></article>`;
}
async function loadData(){
  try{
    const r=await fetch('/data');const d=await r.json();
    document.getElementById('gatewayIp').textContent=d.gateway.ip;
    document.getElementById('gatewaySsid').textContent=d.gateway.ssid;
    document.getElementById('normalCount').textContent=d.gateway.normal_count;
    document.getElementById('warningCount').textContent=d.gateway.warning_count;
    document.getElementById('fireCount').textContent=d.gateway.fire_count;
    const b=document.getElementById('networkBadge');
    if(d.gateway.all_nodes_working){b.innerHTML='<span class="dot"></span><span>All nodes working</span>';b.style.background='#dcfce7';b.style.color='#15803d';}
    else{b.innerHTML='<span class="dot"></span><span>Waiting for all nodes</span>';b.style.background='#fee2e2';b.style.color='#b91c1c';}
    document.getElementById('nodeGrid').innerHTML=d.nodes.map(buildCard).join('');
  }catch(e){console.log(e);}
}
loadData();setInterval(loadData,3000);
</script>
</body>
</html>
)rawliteral";
  server.send(200, "text/html", html);
}

void handleData() {
  server.send(200, "application/json", buildJson());
}

// =====================================================
// SETUP
// =====================================================
void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  for (int i = 1; i <= 4; i++) {
    nodes[i].online = false;
    nodes[i].passkey = "";
    nodes[i].nodeId = i;
    nodes[i].lastSender = 0;
    nodes[i].seq = 0;
    nodes[i].hops = 0;
    nodes[i].temp = "-";
    nodes[i].humid = "-";
    nodes[i].smoke = "-";
    nodes[i].fire = "-";
    nodes[i].lat = "-";
    nodes[i].lng = "-";
    nodes[i].health = "-";
    nodes[i].path = "-";
    nodes[i].category = "Normal";
    nodes[i].rssi = 0;
    nodes[i].lastSeenMillis = 0;

    // Init category history
    for (int j = 0; j < STABILITY_WINDOW; j++) {
      categoryHistory[i][j] = "Normal";
    }
  }

  // Init unregistered nodes
  for (int i = 0; i < MAX_UNREG; i++) {
    unregNodes[i].active = false;
    unregNodes[i].lastSeenMillis = 0;
  }

  // LoRa init with MAXIMUM RANGE settings
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);
  if (!LoRa.begin(433E6)) {
    Serial.println("LoRa init failed!");
    while (1) {
      digitalWrite(LED_PIN, !digitalRead(LED_PIN));
      delay(120);
    }
  }

  // ===== LORA SETTINGS — SF10 matches nodes =====
  LoRa.setSpreadingFactor(10);
  LoRa.setSignalBandwidth(125E3);
  LoRa.setCodingRate4(8);                          // 4/8 = maximum error correction
  LoRa.setTxPower(20, PA_OUTPUT_PA_BOOST_PIN);
  LoRa.setPreambleLength(8);
  LoRa.enableCrc();
  LoRa.setGain(0);

  Serial.println("LoRa: SF10, BW125k, CR4/8, TX20dBm, CRC ON");

  // Put LoRa in continuous receive mode
  LoRa.receive();

  // WiFi: AP + Station mode
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASS);

  // Connect to internet WiFi for Firebase
  Serial.print("Connecting to WiFi: ");
  Serial.println(STA_SSID);
  WiFi.begin(STA_SSID, STA_PASS);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.print("WiFi connected! IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println();
    Serial.println("WiFi connection failed — Firebase push disabled");
    Serial.println("Local gateway dashboard still works via AP");
  }

  Serial.println("--------------------------------");
  Serial.println("Breadboard 2 Gateway Ready (Beacon + Firebase + Mesh)");
  Serial.print("Gateway AP IP: ");
  Serial.println(WiFi.softAPIP());
  Serial.println("Beacon interval: " + String(BEACON_INTERVAL_MS / 1000) + "s");

  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.begin();

  Serial.println("Local website ready");
  Serial.println("Connect to WiFi: CENSYS-Gateway");
  Serial.println("Open browser: http://192.168.4.1");

  // Send first beacon immediately
  sendBeacon();
  lastBeaconSent = millis();
}

// =====================================================
// LOOP
// =====================================================
void loop() {
  server.handleClient();
  updateGatewayLED();

  // ===== BEACON BROADCAST =====
  // Send a short beacon every BEACON_INTERVAL_MS so nodes can discover the gateway
  if (millis() - lastBeaconSent >= BEACON_INTERVAL_MS) {
    sendBeacon();
    lastBeaconSent = millis();
  }

  // Firebase: push ONE item per cycle (non-blocking round-robin)
  // This is critical: pushing all 5+ items at once blocks LoRa for 5-10 seconds!
  if (firebaseDataDirty && (millis() - lastFirebasePush >= FIREBASE_PUSH_INTERVAL_MS)) {
    pushNextToFirebase();
    lastFirebasePush = millis();
    lastStatusPush = millis();
  }

  // Periodic status push (even without new data) so offline nodes get updated
  if (millis() - lastStatusPush >= FIREBASE_STATUS_PUSH_MS) {
    firebaseDataDirty = true;
    firebasePushIndex = 0;  // Restart round-robin
  }

  int packetSize = LoRa.parsePacket();
  if (!packetSize) return;

  String rx = "";
  while (LoRa.available()) rx += (char)LoRa.read();
  rx.trim();

  int rssi = LoRa.packetRssi();

  // Quick check: reject obviously too-short or too-long packets
  if (rx.length() < 10 || rx.length() > 500) {
    Serial.println("--------------------------------");
    Serial.println("Rejected packet: Invalid length (" + String(rx.length()) + ")");
    return;
  }

  String key  = getField(rx, 0);
  String type = getField(rx, 1);

  // Ignore our own beacons bouncing back
  if (type == "BEACON") {
    return;
  }

  if (type != "DATA") {
    // Could be an ACK or other type — ignore silently
    return;
  }

  String originStr = getField(rx, 2);
  String senderStr = getField(rx, 3);
  String seqStr    = getField(rx, 4);
  String hopsStr   = getField(rx, 5);
  String tempStr   = getField(rx, 6);
  String humStr    = getField(rx, 7);
  String mq2Str    = getField(rx, 8);
  String fireStr   = getField(rx, 9);
  String latStr    = getField(rx, 10);
  String lngStr    = getField(rx, 11);
  String healthStr = getField(rx, 12);
  String pathStr   = getField(rx, 13);

  // ===== NOISE FILTER: reject garbage data =====
  if (isNoisePacket(rx, tempStr, humStr, mq2Str, fireStr, originStr, latStr, lngStr)) {
    Serial.println("--------------------------------");
    Serial.println("REJECTED: Noise/garbage packet detected");
    Serial.println("Raw: " + rx.substring(0, min((int)rx.length(), 80)));
    return;
  }

  int originNode = originStr.toInt();
  int lastSender = senderStr.toInt();
  unsigned long seq = seqStr.toInt();
  int hops = hopsStr.toInt();

  // ===== HANDLE UNREGISTERED NODES =====
  if (!isRegisteredKey(key)) {
    Serial.println("--------------------------------");
    Serial.println("UNREGISTERED NODE detected (valid data, unknown passkey)");
    Serial.print("Passkey: "); Serial.println(key);
    Serial.print("Node ID: "); Serial.println(originNode);

    int slot = findOrAddUnreg(key, originNode);
    unregNodes[slot].active = true;
    unregNodes[slot].passkey = key;
    unregNodes[slot].nodeId = originNode;
    unregNodes[slot].temp = tempStr;
    unregNodes[slot].humid = humStr;
    unregNodes[slot].smoke = mq2Str;
    unregNodes[slot].fire = fireStr;
    unregNodes[slot].health = healthStr;
    unregNodes[slot].rssi = rssi;
    unregNodes[slot].hops = hops;
    unregNodes[slot].path = pathStr;
    unregNodes[slot].lastSeenMillis = millis();

    firebaseUnregDirty = true;
    firebaseDataDirty = true;  // Trigger push cycle

    // Do NOT send ACK to unregistered nodes
    return;
  }

  // ===== REGISTERED NODE HANDLING =====
  if (originNode < 1 || originNode > 4) {
    Serial.println("--------------------------------");
    Serial.println("Rejected packet: Invalid node ID");
    return;
  }

  String sig = key + "|" + originStr + "|" + seqStr;
  if (isSeen(sig)) {
    Serial.println("--------------------------------");
    Serial.println("Duplicate packet ignored");
    return;
  }
  addSeen(sig);

  // Classify category
  String rawCategory = classifyCategory(tempStr, humStr, mq2Str, fireStr, healthStr);

  // ===== TEMPORAL STABILITY: prevent false fire from noise =====
  addCategoryHistory(originNode, rawCategory);
  String finalCategory = rawCategory;

  if (rawCategory == "Fire") {
    if (!isFireStable(originNode)) {
      // Not enough consecutive fire/warning readings — downgrade to Warning
      finalCategory = "Warning";
      Serial.println("STABILITY: Fire downgraded to Warning (need 2 of 3 consistent readings)");
    }
  }

  nodes[originNode].online = true;
  nodes[originNode].passkey = key;
  nodes[originNode].nodeId = originNode;
  nodes[originNode].lastSender = lastSender;
  nodes[originNode].seq = seq;
  nodes[originNode].hops = hops;
  nodes[originNode].temp = tempStr;
  nodes[originNode].humid = humStr;
  nodes[originNode].smoke = mq2Str;
  nodes[originNode].fire = fireStr;

  // ===== GPS PERSISTENCE =====
  // Only update GPS if the new value is valid (not 0, not empty, not no-fix)
  // This preserves the last known GPS for both local dashboard and Firebase
  if (isValidGPS(latStr, lngStr)) {
    nodes[originNode].lat = latStr;
    nodes[originNode].lng = lngStr;
    savedLat[originNode] = latStr;
    savedLng[originNode] = lngStr;
    Serial.println("GPS: Updated node" + String(originNode) + " → " + latStr + "," + lngStr);
  } else {
    // Use saved GPS if available, otherwise keep whatever was there
    if (savedLat[originNode].length() > 0) {
      nodes[originNode].lat = savedLat[originNode];
      nodes[originNode].lng = savedLng[originNode];
      Serial.println("GPS: Kept saved for node" + String(originNode) + " (incoming was no-fix/zero)");
    } else {
      nodes[originNode].lat = latStr;
      nodes[originNode].lng = lngStr;
    }
  }

  nodes[originNode].health = healthStr;
  nodes[originNode].path = pathStr;
  nodes[originNode].category = finalCategory;
  nodes[originNode].rssi = rssi;
  nodes[originNode].lastSeenMillis = millis();

  // Mark data dirty so Firebase push happens on next interval
  firebaseDataDirty = true;

  Serial.println("--------------------------------");
  Serial.println("VALID NODE DATA RECEIVED");
  Serial.print("Origin Node: "); Serial.println(originNode);
  Serial.print("Last Sender: "); Serial.println(lastSender);
  Serial.print("Sequence: "); Serial.println(seq);
  Serial.print("Hops: "); Serial.println(hops);
  Serial.print("Temp: "); Serial.println(tempStr);
  Serial.print("Humid: "); Serial.println(humStr);
  Serial.print("Smoke: "); Serial.println(mq2Str);
  Serial.print("Fire Sensor: "); Serial.println(fireStr);
  Serial.print("Latitude: "); Serial.println(latStr);
  Serial.print("Longitude: "); Serial.println(lngStr);
  Serial.print("Health: "); Serial.println(healthStr);
  Serial.print("Raw Category: "); Serial.println(rawCategory);
  Serial.print("Final Category: "); Serial.println(finalCategory);
  Serial.print("Path: "); Serial.println(pathStr);
  Serial.print("RSSI: "); Serial.println(rssi);

  // Send ACK to the original node
  sendAck(key, originNode, seqStr);

  // If this was a relayed packet, also ACK the relay node
  if (hops > 0 && lastSender != originNode) {
    delay(30);  // Small gap between ACKs
    sendAck(key, lastSender, seqStr);
    Serial.println("ACK -> Relay Node " + String(lastSender));
  }
}
