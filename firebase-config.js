// =====================================================
// CENSYS Fire Monitoring — Firebase Configuration
// =====================================================

const firebaseConfig = {
  apiKey: "AIzaSyA7vrQaH6DzLcEb_WKK-uQPRDZQ7wXNRi8",
  authDomain: "censys-antigravity.firebaseapp.com",
  databaseURL: "https://censys-antigravity-default-rtdb.asia-southeast1.firebasedatabase.app",
  projectId: "censys-antigravity",
  storageBucket: "censys-antigravity.firebasestorage.app",
  messagingSenderId: "573513687331",
  appId: "1:573513687331:web:1d703ff2ed971a80bb6c96",
  measurementId: "G-8JNZQ05473"
};

firebase.initializeApp(firebaseConfig);
const database = firebase.database();

// =====================================================
// CONSTANTS
// =====================================================
const NODE_OFFLINE_TIMEOUT_MS = 240000;  // 4 minutes — as required by instructor

// =====================================================
// GPS PERSISTENCE CACHE
// =====================================================
const savedGPS = {};

function isValidGPSValue(val) {
  if (!val || val === '') return false;
  const s = String(val).trim();
  if (s === '0' || s === '0.0' || s === '0.00' || s === '0.000000') return false;
  if (s === '-' || s === '--') return false;
  if (s.toLowerCase().indexOf('no fix') >= 0) return false;
  const num = parseFloat(s);
  if (isNaN(num) || num === 0) return false;
  return true;
}

// =====================================================
// BARANGAY DATA — Boundary polygons traced from Google Maps
// Used for: map overlays AND point-in-polygon detection
// =====================================================
const BARANGAY_DATA = {
  kalunasan: {
    name: 'Barangay Kalunasan',
    center: [10.3320, 123.8830],
    zoom: 15,
    boundary: [
      [10.3415, 123.8759],
      [10.3410, 123.8805],
      [10.3395, 123.8842],
      [10.3380, 123.8870],
      [10.3350, 123.8895],
      [10.3318, 123.8905],
      [10.3290, 123.8895],
      [10.3258, 123.8859],
      [10.3245, 123.8841],
      [10.3245, 123.8800],
      [10.3260, 123.8775],
      [10.3290, 123.8760],
      [10.3310, 123.8758],
      [10.3340, 123.8755],
      [10.3370, 123.8752],
      [10.3392, 123.8755]
    ]
  },
  sannicolas: {
    name: 'Barangay San Nicolas',
    center: [10.2942, 123.8905],
    zoom: 17,
    boundary: [
      [10.2962, 123.8896],
      [10.2960, 123.8918],
      [10.2952, 123.8930],
      [10.2940, 123.8928],
      [10.2927, 123.8924],
      [10.2917, 123.8910],
      [10.2920, 123.8897],
      [10.2927, 123.8885],
      [10.2934, 123.8872],
      [10.2945, 123.8864],
      [10.2955, 123.8870],
      [10.2960, 123.8880]
    ]
  },
  kalubihan: {
    name: 'Barangay Kalubihan',
    center: [10.2970, 123.8980],
    zoom: 17,
    boundary: [
      [10.2990, 123.8963],
      [10.2989, 123.8980],
      [10.2985, 123.8998],
      [10.2978, 123.9004],
      [10.2965, 123.9003],
      [10.2952, 123.8993],
      [10.2951, 123.8978],
      [10.2958, 123.8963],
      [10.2970, 123.8955],
      [10.2980, 123.8957]
    ]
  }
};

// =====================================================
// POINT-IN-POLYGON (Ray casting algorithm)
// polygon = array of [lat, lng] pairs
// =====================================================
function pointInPolygon(lat, lng, polygon) {
  let inside = false;
  for (let i = 0, j = polygon.length - 1; i < polygon.length; j = i++) {
    const yi = polygon[i][0], xi = polygon[i][1];
    const yj = polygon[j][0], xj = polygon[j][1];
    const intersect = ((yi > lat) !== (yj > lat)) &&
      (lng < (xj - xi) * (lat - yi) / (yj - yi) + xi);
    if (intersect) inside = !inside;
  }
  return inside;
}

// =====================================================
// GET BARANGAY FROM GPS COORDINATES
// Checks all polygons, falls back to nearest center
// =====================================================
function getBarangayFromGPS(lat, lng) {
  lat = parseFloat(lat);
  lng = parseFloat(lng);
  if (isNaN(lat) || isNaN(lng) || lat === 0 || lng === 0) return null;

  // Check polygons first
  for (const [key, brgy] of Object.entries(BARANGAY_DATA)) {
    if (pointInPolygon(lat, lng, brgy.boundary)) {
      return key;
    }
  }

  // Fallback: nearest center (handles GPS jitter at edges)
  let nearest = null;
  let minDist = Infinity;
  for (const [key, brgy] of Object.entries(BARANGAY_DATA)) {
    const d = haversineDistance(lat, lng, brgy.center[0], brgy.center[1]);
    if (d < minDist) {
      minDist = d;
      nearest = key;
    }
  }
  // Only assign if within 800m of center (reasonable barangay radius)
  if (minDist < 800) return nearest;
  return null;
}

// =====================================================
// FIREBASE LISTENER — reads from /barangays/ paths
// =====================================================
function listenToNodes(callback) {
  const ref = database.ref('barangays');
  ref.on('value', (snapshot) => {
    const barangaysData = snapshot.val() || {};
    const now = Date.now();
    const mergedNodes = {};

    Object.keys(barangaysData).forEach(brgyKey => {
      const brgyNodes = barangaysData[brgyKey] || {};
      Object.keys(brgyNodes).forEach(nodeKey => {
        const node = brgyNodes[nodeKey];
        if (!node) return;

        node.barangay = brgyKey;
        const mergedKey = brgyKey + '_' + nodeKey;

        const hasData = (node.temp && node.temp !== '') ||
                        (node.humid && node.humid !== '') ||
                        (node.smoke && node.smoke !== '') ||
                        (node.fire && node.fire !== '');

        if (node.timestamp) {
          const age = now - node.timestamp;
          node.last_seen_sec = Math.round(age / 1000);
          node.online = (age <= NODE_OFFLINE_TIMEOUT_MS && hasData);
        } else {
          node.online = false;
        }

        // GPS persistence
        const hasValidLat = isValidGPSValue(node.lat);
        const hasValidLng = isValidGPSValue(node.lng);
        if (hasValidLat && hasValidLng) {
          savedGPS[mergedKey] = { lat: node.lat, lng: node.lng };
        } else if (savedGPS[mergedKey]) {
          node.lat = savedGPS[mergedKey].lat;
          node.lng = savedGPS[mergedKey].lng;
        }

        mergedNodes[mergedKey] = node;
      });
    });

    callback(mergedNodes);
  });
  return () => ref.off('value');
}

function listenToGateway(callback) {
  const ref = database.ref('gateway');
  ref.on('value', (snapshot) => callback(snapshot.val() || {}));
  return () => ref.off('value');
}

function listenToUnregisteredNodes(callback) {
  const ref = database.ref('unregistered_nodes');
  ref.on('value', (snapshot) => callback(snapshot.val() || {}));
  return () => ref.off('value');
}

// =====================================================
// FIRE STATUS LISTENER — reads fire_status flags
// =====================================================
function listenToFireStatus(callback) {
  const ref = database.ref('fire_status');
  ref.on('value', (snapshot) => callback(snapshot.val() || {}));
  return () => ref.off('value');
}

// =====================================================
// CLEAR FIRE ALARM — operator confirms fire is out
// Writes fire_cleared:true to Firebase so gateway/nodes unlock
// =====================================================
function clearFireAlarm(barangay) {
  return database.ref('fire_status/' + barangay).update({
    fire_cleared: true,
    cleared_at: firebase.database.ServerValue.TIMESTAMP,
    cleared_by: sessionStorage.getItem('user') || 'operator'
  });
}

// =====================================================
// LOG FIRE INCIDENT — records for history
// =====================================================
function logFireIncident(data) {
  return database.ref('fire_incidents').push({
    ...data,
    timestamp: firebase.database.ServerValue.TIMESTAMP
  });
}

// =====================================================
// LISTEN TO FIRE INCIDENTS — for historical log
// =====================================================
function listenToFireIncidents(callback, limit = 20) {
  const ref = database.ref('fire_incidents').orderByChild('timestamp').limitToLast(limit);
  ref.on('value', (snapshot) => {
    const incidents = [];
    snapshot.forEach(child => {
      incidents.push({ id: child.key, ...child.val() });
    });
    // Reverse so newest is first
    incidents.reverse();
    callback(incidents);
  });
  return () => ref.off('value');
}

// =====================================================
// DELETE UNREGISTERED NODE (when it gets a barangay)
// =====================================================
function deleteUnregisteredNode(nodeKey) {
  return database.ref('unregistered_nodes/' + nodeKey).remove();
}

// =====================================================
// Session helpers
// =====================================================
function getSession() {
  return {
    role: sessionStorage.getItem('role'),
    barangay: sessionStorage.getItem('barangay'),
    user: sessionStorage.getItem('user')
  };
}

function setSession(role, barangay, user) {
  sessionStorage.setItem('role', role);
  sessionStorage.setItem('barangay', barangay || '');
  sessionStorage.setItem('user', user || '');
}

function clearSession() {
  sessionStorage.removeItem('role');
  sessionStorage.removeItem('barangay');
  sessionStorage.removeItem('user');
}

function requireAuth(requiredRole) {
  const session = getSession();
  if (!session.role || session.role !== requiredRole) {
    window.location.href = 'index.html';
    return false;
  }
  return true;
}

// =====================================================
// Fire Alarm Sound (Web Audio API)
// =====================================================
let alarmAudioCtx = null;
let alarmOscillator = null;
let alarmGain = null;
let alarmPlaying = false;
let alarmSilenced = false;

function startAlarmSound() {
  if (alarmPlaying) return;
  try {
    alarmAudioCtx = new (window.AudioContext || window.webkitAudioContext)();
    alarmGain = alarmAudioCtx.createGain();
    alarmGain.gain.value = 0.3;
    alarmGain.connect(alarmAudioCtx.destination);
    alarmOscillator = alarmAudioCtx.createOscillator();
    alarmOscillator.type = 'sawtooth';
    alarmOscillator.frequency.value = 800;
    alarmOscillator.connect(alarmGain);
    alarmOscillator.start();
    function sweep() {
      if (!alarmPlaying) return;
      const now = alarmAudioCtx.currentTime;
      alarmOscillator.frequency.setValueAtTime(800, now);
      alarmOscillator.frequency.linearRampToValueAtTime(1200, now + 0.5);
      alarmOscillator.frequency.linearRampToValueAtTime(800, now + 1.0);
      setTimeout(sweep, 1000);
    }
    alarmPlaying = true;
    sweep();
  } catch (e) {
    console.warn('Audio alarm failed:', e);
  }
}

function stopAlarmSound() {
  alarmPlaying = false;
  if (alarmOscillator) { try { alarmOscillator.stop(); } catch(e) {} alarmOscillator = null; }
  if (alarmAudioCtx) { try { alarmAudioCtx.close(); } catch(e) {} alarmAudioCtx = null; }
}

function silenceAlarm() { alarmSilenced = true; stopAlarmSound(); }
function resetAlarmSilence() { alarmSilenced = false; }

// =====================================================
// Haversine Distance (meters)
// =====================================================
function haversineDistance(lat1, lng1, lat2, lng2) {
  const R = 6371000;
  const dLat = (lat2 - lat1) * Math.PI / 180;
  const dLng = (lng2 - lng1) * Math.PI / 180;
  const a = Math.sin(dLat / 2) * Math.sin(dLat / 2) +
            Math.cos(lat1 * Math.PI / 180) * Math.cos(lat2 * Math.PI / 180) *
            Math.sin(dLng / 2) * Math.sin(dLng / 2);
  return R * 2 * Math.atan2(Math.sqrt(a), Math.sqrt(1 - a));
}

// =====================================================
// Fire Proximity Clustering
// =====================================================
function clusterFires(nodesData, radiusMeters = 100) {
  const fireNodes = Object.values(nodesData).filter(n =>
    n.online && n.category === 'Fire' &&
    parseFloat(n.lat) !== 0 && parseFloat(n.lng) !== 0
  );
  if (fireNodes.length === 0) return { clusters: [], totalIncidents: 0, totalFireNodes: 0 };

  const parent = fireNodes.map((_, i) => i);
  function find(x) { while (parent[x] !== x) { parent[x] = parent[parent[x]]; x = parent[x]; } return x; }
  function union(a, b) { const ra = find(a), rb = find(b); if (ra !== rb) parent[ra] = rb; }

  for (let i = 0; i < fireNodes.length; i++) {
    for (let j = i + 1; j < fireNodes.length; j++) {
      if (haversineDistance(parseFloat(fireNodes[i].lat), parseFloat(fireNodes[i].lng),
        parseFloat(fireNodes[j].lat), parseFloat(fireNodes[j].lng)) <= radiusMeters) {
        union(i, j);
      }
    }
  }

  const clusterMap = {};
  fireNodes.forEach((node, i) => {
    const root = find(i);
    if (!clusterMap[root]) clusterMap[root] = [];
    clusterMap[root].push(node);
  });

  const clusters = Object.values(clusterMap);
  return { clusters, totalIncidents: clusters.length, totalFireNodes: fireNodes.length };
}

// =====================================================
// BFP Fire Alarm Levels (7 levels)
// =====================================================
function getBFPAlarmLevel(fireIncidentCount) {
  if (fireIncidentCount <= 0) return { level: 0, label: 'No Alarm', class: 'alarm-level--none' };
  if (fireIncidentCount === 1) return { level: 1, label: '1st Alarm', class: 'alarm-level--1' };
  if (fireIncidentCount <= 3) return { level: 2, label: '2nd Alarm', class: 'alarm-level--2' };
  if (fireIncidentCount <= 5) return { level: 3, label: '3rd Alarm', class: 'alarm-level--3' };
  if (fireIncidentCount <= 7) return { level: 4, label: '4th Alarm', class: 'alarm-level--4' };
  if (fireIncidentCount <= 9) return { level: 5, label: '5th Alarm', class: 'alarm-level--5' };
  if (fireIncidentCount <= 11) return { level: 6, label: 'Task Force Alpha', class: 'alarm-level--tfa' };
  return { level: 7, label: 'Task Force Bravo', class: 'alarm-level--tfb' };
}

// =====================================================
// UI Helpers
// =====================================================
function categoryBadgeClass(cat) {
  if (cat === 'Fire') return 'badge--fire';
  if (cat === 'Warning') return 'badge--warn';
  return 'badge--ok';
}

function statusBadgeClass(online) {
  return online ? 'badge--ok' : 'badge--offline';
}

function safe(v) {
  if (v === undefined || v === null || v === '') return '\u2014';
  return String(v);
}

function formatTimeAgo(seconds) {
  if (seconds === undefined || seconds === null) return '\u2014';
  if (seconds < 5) return 'just now';
  if (seconds < 60) return seconds + 's ago';
  if (seconds < 3600) return Math.floor(seconds / 60) + 'm ago';
  if (seconds < 86400) return Math.floor(seconds / 3600) + 'h ago';
  return Math.floor(seconds / 86400) + 'd ago';
}

// =====================================================
// RSSI Signal Quality
// =====================================================
function getSignalQuality(rssi) {
  if (rssi === undefined || rssi === null || rssi === 0) return { label: '\u2014', class: 'sig-none', bars: 0 };
  if (rssi >= -70) return { label: 'Excellent', class: 'sig-great', bars: 4 };
  if (rssi >= -90) return { label: 'Good', class: 'sig-good', bars: 3 };
  if (rssi >= -110) return { label: 'Fair', class: 'sig-fair', bars: 2 };
  return { label: 'Weak', class: 'sig-weak', bars: 1 };
}

function signalBarsHTML(rssi) {
  const sig = getSignalQuality(rssi);
  if (sig.bars === 0) return '<span style="color:var(--text-muted)">\u2014</span>';
  let bars = '';
  for (let i = 1; i <= 4; i++) {
    const h = 4 + i * 4;
    const active = i <= sig.bars;
    bars += `<span style="display:inline-block;width:4px;height:${h}px;margin:0 1px;border-radius:1px;vertical-align:bottom;background:${active ? (sig.bars >= 3 ? 'var(--ok)' : sig.bars >= 2 ? 'var(--warn)' : 'var(--fire)') : 'var(--border)'}"></span>`;
  }
  return `<span title="${sig.label} (${rssi} dBm)">${bars}</span>`;
}

// =====================================================
// Live clock
// =====================================================
function startLiveClock(elementId) {
  function update() {
    const el = document.getElementById(elementId);
    if (!el) return;
    el.textContent = new Date().toLocaleTimeString('en-PH', { hour: '2-digit', minute: '2-digit', second: '2-digit', hour12: true });
  }
  update();
  setInterval(update, 1000);
}
