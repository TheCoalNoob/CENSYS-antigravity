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
// Returns an unsubscribe function
// =====================================================
function listenToNodes(callback) {
  const ref = database.ref('nodes');
  ref.on('value', (snapshot) => {
    const data = snapshot.val() || {};
    callback(data);
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
// BFP Fire Alarm Level Calculator
// =====================================================
function getBFPAlarmLevel(fireCount) {
  if (fireCount <= 0) return { level: 0, label: 'No Alarm', class: 'alarm-level--none' };
  if (fireCount === 1) return { level: 1, label: '1st Alarm', class: 'alarm-level--1' };
  if (fireCount <= 3) return { level: 2, label: '2nd Alarm', class: 'alarm-level--2' };
  if (fireCount <= 5) return { level: 3, label: '3rd Alarm', class: 'alarm-level--3' };
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
// Barangay metadata
// =====================================================
const BARANGAY_DATA = {
  kalunasan: {
    name: 'Barangay Kalunasan',
    center: [10.3341116, 123.8815901],
    zoom: 15
  },
  sannicolas: {
    name: 'Barangay San Nicolas',
    center: [10.2947462, 123.8893411],
    zoom: 17
  },
  kalubihan: {
    name: 'Barangay Kalubihan',
    center: [10.2968983, 123.8979788],
    zoom: 17
  }
};
