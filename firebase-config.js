// =====================================================
// CENSYS Fire Monitoring — Firebase Configuration
// Replace the placeholder values below with your
// actual Firebase project credentials.
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

// Initialize Firebase
firebase.initializeApp(firebaseConfig);
const database = firebase.database();

// =====================================================
// Shared helper: listen to all nodes
// IMPORTANT: Online status is determined by timestamp.
// If a node hasn't sent data in 2 minutes, it's OFFLINE
// regardless of the "online" field in Firebase.
// This prevents "ghost online" nodes after power-off.
// =====================================================
const NODE_OFFLINE_TIMEOUT_MS = 120000;  // 2 minutes

// =====================================================
// GPS PERSISTENCE CACHE
// Stores the last known valid GPS for each node so the
// website always shows a position even if the node sends
// "No Fix" or 0.0 coordinates.
// =====================================================
const savedGPS = {};  // { nodeKey: { lat: '...', lng: '...' } }

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

function listenToNodes(callback) {
  const ref = database.ref('barangays');
  ref.on('value', (snapshot) => {
    const barangaysData = snapshot.val() || {};
    const now = Date.now();
    const mergedNodes = {};

    // Iterate each barangay and merge nodes into a flat map
    // Key format: {barangay}_node{id} to keep nodes from different barangays separate
    Object.keys(barangaysData).forEach(brgyKey => {
      const brgyNodes = barangaysData[brgyKey] || {};
      Object.keys(brgyNodes).forEach(nodeKey => {
        const node = brgyNodes[nodeKey];
        if (!node) return;

        // Set barangay from the path, not from the node's field
        node.barangay = brgyKey;

        const mergedKey = brgyKey + '_' + nodeKey;

        // Check if all sensor data is blank/empty — treat as offline
        const hasData = (node.temp && node.temp !== '') ||
                        (node.humid && node.humid !== '') ||
                        (node.smoke && node.smoke !== '') ||
                        (node.fire && node.fire !== '');

        if (node.timestamp) {
          const age = now - node.timestamp;
          node.last_seen_sec = Math.round(age / 1000);

          if (age > NODE_OFFLINE_TIMEOUT_MS || !hasData) {
            node.online = false;
          } else {
            node.online = true;
          }
        } else {
          node.online = false;
        }

        // ===== GPS PERSISTENCE =====
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

// Listen to gateway status
function listenToGateway(callback) {
  const ref = database.ref('gateway');
  ref.on('value', (snapshot) => {
    const data = snapshot.val() || {};
    callback(data);
  });
  return () => ref.off('value');
}

// Listen to unregistered nodes (nodes with unknown passkeys)
function listenToUnregisteredNodes(callback) {
  const ref = database.ref('unregistered_nodes');
  ref.on('value', (snapshot) => {
    const data = snapshot.val() || {};
    callback(data);
  });
  return () => ref.off('value');
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
// Fire Alarm Sound (Web Audio API — no file needed)
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

    // Create alternating siren
    alarmOscillator = alarmAudioCtx.createOscillator();
    alarmOscillator.type = 'sawtooth';
    alarmOscillator.frequency.value = 800;
    alarmOscillator.connect(alarmGain);
    alarmOscillator.start();

    // Siren sweep
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
  if (alarmOscillator) {
    try { alarmOscillator.stop(); } catch(e) {}
    alarmOscillator = null;
  }
  if (alarmAudioCtx) {
    try { alarmAudioCtx.close(); } catch(e) {}
    alarmAudioCtx = null;
  }
}

function silenceAlarm() {
  alarmSilenced = true;
  stopAlarmSound();
}

function resetAlarmSilence() {
  alarmSilenced = false;
}

// =====================================================
// Haversine Distance (meters) between two GPS coords
// =====================================================
function haversineDistance(lat1, lng1, lat2, lng2) {
  const R = 6371000; // Earth radius in meters
  const dLat = (lat2 - lat1) * Math.PI / 180;
  const dLng = (lng2 - lng1) * Math.PI / 180;
  const a = Math.sin(dLat / 2) * Math.sin(dLat / 2) +
            Math.cos(lat1 * Math.PI / 180) * Math.cos(lat2 * Math.PI / 180) *
            Math.sin(dLng / 2) * Math.sin(dLng / 2);
  const c = 2 * Math.atan2(Math.sqrt(a), Math.sqrt(1 - a));
  return R * c;
}

// =====================================================
// Fire Proximity Clustering
// Groups fire nodes within radiusMeters into clusters.
// Each cluster = 1 fire incident for BFP alarm levels.
// =====================================================
function clusterFires(nodesData, radiusMeters = 100) {
  const fireNodes = Object.values(nodesData).filter(n =>
    n.online && n.category === 'Fire' &&
    parseFloat(n.lat) !== 0 && parseFloat(n.lng) !== 0
  );

  if (fireNodes.length === 0) return { clusters: [], totalIncidents: 0, totalFireNodes: 0 };

  // Union-Find for clustering
  const parent = fireNodes.map((_, i) => i);
  function find(x) {
    while (parent[x] !== x) { parent[x] = parent[parent[x]]; x = parent[x]; }
    return x;
  }
  function union(a, b) {
    const ra = find(a), rb = find(b);
    if (ra !== rb) parent[ra] = rb;
  }

  // Group nodes within radius
  for (let i = 0; i < fireNodes.length; i++) {
    for (let j = i + 1; j < fireNodes.length; j++) {
      const dist = haversineDistance(
        parseFloat(fireNodes[i].lat), parseFloat(fireNodes[i].lng),
        parseFloat(fireNodes[j].lat), parseFloat(fireNodes[j].lng)
      );
      if (dist <= radiusMeters) {
        union(i, j);
      }
    }
  }

  // Build clusters
  const clusterMap = {};
  fireNodes.forEach((node, i) => {
    const root = find(i);
    if (!clusterMap[root]) clusterMap[root] = [];
    clusterMap[root].push(node);
  });

  const clusters = Object.values(clusterMap);
  return {
    clusters,
    totalIncidents: clusters.length,
    totalFireNodes: fireNodes.length
  };
}

// =====================================================
// BFP Fire Alarm Level Calculator (uses cluster count)
// =====================================================
function getBFPAlarmLevel(fireIncidentCount) {
  if (fireIncidentCount <= 0) return { level: 0, label: 'No Alarm', class: 'alarm-level--none' };
  if (fireIncidentCount === 1) return { level: 1, label: '1st Alarm', class: 'alarm-level--1' };
  if (fireIncidentCount <= 3) return { level: 2, label: '2nd Alarm', class: 'alarm-level--2' };
  if (fireIncidentCount <= 5) return { level: 3, label: '3rd Alarm', class: 'alarm-level--3' };
  return { level: 4, label: 'General Alarm', class: 'alarm-level--general' };
}

// =====================================================
// Category badge helper
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
  if (v === undefined || v === null || v === '') return '—';
  return String(v);
}

// =====================================================
// Human-readable time ago
// =====================================================
function formatTimeAgo(seconds) {
  if (seconds === undefined || seconds === null) return '—';
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
  if (rssi === undefined || rssi === null || rssi === 0) return { label: '—', class: 'sig-none', bars: 0 };
  if (rssi >= -70) return { label: 'Excellent', class: 'sig-great', bars: 4 };
  if (rssi >= -90) return { label: 'Good', class: 'sig-good', bars: 3 };
  if (rssi >= -110) return { label: 'Fair', class: 'sig-fair', bars: 2 };
  return { label: 'Weak', class: 'sig-weak', bars: 1 };
}

function signalBarsHTML(rssi) {
  const sig = getSignalQuality(rssi);
  if (sig.bars === 0) return '<span style="color:var(--text-muted)">—</span>';
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
    const now = new Date();
    el.textContent = now.toLocaleTimeString('en-PH', { hour: '2-digit', minute: '2-digit', second: '2-digit', hour12: true });
  }
  update();
  setInterval(update, 1000);
}

// =====================================================
// Barangay metadata
// =====================================================
const BARANGAY_DATA = {
  kalunasan: {
    name: 'Barangay Kalunasan',
    center: [10.3290849, 123.8869029],
    zoom: 16
  },
  sannicolas: {
    name: 'Barangay San Nicolas',
    center: [10.295138, 123.8907164],
    zoom: 17
  },
  kalubihan: {
    name: 'Barangay Kalubihan',
    center: [10.2991276, 123.8956305],
    zoom: 17
  }
};
