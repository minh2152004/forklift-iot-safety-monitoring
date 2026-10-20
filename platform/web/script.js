const METRICS = {
  weight_kg: { label: 'Weight', unit: 'kg', sub: 'Real-time weight readings',     color: '#3b82f6', precision: 2 },
  tilt:      { label: 'Tilt',   unit: '°',  sub: 'Combined tilt angle (roll/pitch)', color: '#f97316', precision: 2 }
};

let activeMetric = 'weight_kg';
let activeRange  = 15;
let chart;
let cachedData   = [];
let pollTimer    = null;
let locationPoll = null;
let cameraStream = null;
let lastFrameRender = 0;
const FRAME_INTERVAL_MS = 2000;   // throttle camera frames to ~1 per 2s

// ============================================================
// CHART INITIALISATION
// ============================================================
function initChart() {
  const ctx = document.getElementById('sensorChart').getContext('2d');
  chart = new Chart(ctx, {
    type: 'line',
    data: {
      labels: [],
      datasets: [{
        label: METRICS[activeMetric].label,
        data: [],
        borderColor: METRICS[activeMetric].color,
        backgroundColor: hexToRgba(METRICS[activeMetric].color, 0.08),
        borderWidth: 2,
        pointRadius: 0,
        pointHoverRadius: 5,
        pointHoverBackgroundColor: METRICS[activeMetric].color,
        pointHoverBorderColor: '#fff',
        pointHoverBorderWidth: 2,
        tension: 0.25,
        fill: true
      }]
    },
    options: {
      responsive: true,
      maintainAspectRatio: false,
      interaction: { mode: 'index', intersect: false },
      layout: {
        padding: { left: 8, right: 16, top: 8, bottom: 4 }
      },
      plugins: {
        legend: { display: false },
        tooltip: {
          backgroundColor: 'rgba(11, 14, 26, 0.95)',
          titleColor: '#e8ebf3',
          bodyColor: '#a8b0c5',
          borderColor: '#2a3354',
          borderWidth: 1,
          padding: 10,
          titleFont: { family: 'DM Sans', weight: '600', size: 12 },
          bodyFont:  { family: 'JetBrains Mono', size: 12 },
          callbacks: {
            label: (ctx) => {
              const m = METRICS[activeMetric];
              return `${m.label}: ${ctx.parsed.y.toFixed(m.precision)} ${m.unit}`;
            }
          }
        }
      },
      scales: {
        x: {
          grid: { color: 'rgba(31, 38, 64, 0.5)', drawTicks: false },
          ticks: {
            color: '#6b7395',
            font: { family: 'JetBrains Mono', size: 11 },
            padding: 6,
            maxRotation: 0,
            autoSkipPadding: 16
          }
        },
        y: {
          grid: { color: 'rgba(31, 38, 64, 0.5)', drawTicks: false },
          ticks: {
            color: '#6b7395',
            font: { family: 'JetBrains Mono', size: 11 },
            padding: 10
          }
        }
      },
      animation: { duration: 400, easing: 'easeOutQuart' }
    }
  });
}

// ============================================================
// DATA FETCH + RENDER
// ============================================================
async function fetchData() {
  try {
    const res = await fetch(`/api/data?range=${activeRange}`, { cache: 'no-store' });
    const rows = await res.json();
    cachedData = rows;
    updateChart();
    updateLiveValues();
    updateStatus();
    document.getElementById('lastUpdated').textContent =
      'Last updated: ' + new Date().toLocaleTimeString();
  } catch (err) {
    console.error('Fetch error:', err);
    setStatus('danger', 'Connection lost');
  }
}

function updateChart() {
  const m = METRICS[activeMetric];
  // InfluxDB returns one row per field; pivot to one point per timestamp
  const byTime = {};
  for (const r of cachedData) {
    const t = r._time;
    if (!byTime[t]) byTime[t] = {};
    byTime[t][r._field] = r._value;
  }
  const times = Object.keys(byTime).sort();
  const labels = times.map(t => {
    const d = new Date(t);
    return d.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' });
  });
  const values = times.map(t => byTime[t][activeMetric]).filter(v => v !== undefined);

  chart.data.labels = labels;
  chart.data.datasets[0].label = m.label;
  chart.data.datasets[0].data = values;
  chart.data.datasets[0].borderColor = m.color;
  chart.data.datasets[0].backgroundColor = hexToRgba(m.color, 0.08);
  chart.update('none');

  document.getElementById('chartTitle').textContent = `${m.label} (${m.unit})`;
  document.getElementById('chartSubtitle').textContent = m.sub;

  updateStatsStrip(values, m);
}

function updateStatsStrip(values, m) {
  if (values.length === 0) {
    document.getElementById('statMinVal').textContent = '--';
    document.getElementById('statMaxVal').textContent = '--';
    return;
  }
  const min = Math.min(...values);
  const max = Math.max(...values);
  document.getElementById('statMinVal').textContent = min.toFixed(m.precision) + ' ' + m.unit;
  document.getElementById('statMaxVal').textContent = max.toFixed(m.precision) + ' ' + m.unit;
}

function updateLiveValues() {
  // latest reading per metric
  if (cachedData.length === 0) return;
  const byTime = {};
  for (const r of cachedData) {
    const t = r._time;
    if (!byTime[t]) byTime[t] = {};
    byTime[t][r._field] = r._value;
  }
  const latestT = Object.keys(byTime).sort().pop();
  const latest = byTime[latestT] || {};
  for (const key of ['weight_kg', 'tilt']) {
    const el = document.getElementById('liveVal-' + key);
    if (!el) continue;
    const v = latest[key];
    el.textContent = (v !== undefined) ? v.toFixed(METRICS[key].precision) : '--';
  }
}

function updateStatus() {
  if (cachedData.length === 0) {
    setStatus('', 'No data');
    return;
  }
  // pull latest 'status' field
  const statusRows = cachedData.filter(r => r._field === 'status');
  if (statusRows.length === 0) {
    setStatus('ok', 'Connected');
    return;
  }
  const latest = statusRows[statusRows.length - 1]._value;
  if (latest === 'overload' || latest === 'unbalance') {
    setStatus('danger', latest.charAt(0).toUpperCase() + latest.slice(1));
  } else if (latest === 'normal') {
    setStatus('ok', 'Normal');
  } else {
    setStatus('warn', latest);
  }
}

function setStatus(level, text) {
  const dot = document.getElementById('statusLight');
  const txt = document.getElementById('statusText');
  dot.className = 'status-dot' + (level ? ' ' + level : '');
  txt.textContent = text;
}

// ============================================================
// METRIC + TIME RANGE SELECTION
// ============================================================
function selectMetric(metric) {
  activeMetric = metric;
  document.querySelectorAll('.metric-card').forEach(c => {
    c.classList.toggle('active', c.dataset.metric === metric);
  });
  updateChart();
}

function selectRange(range) {
  activeRange = parseInt(range);
  document.querySelectorAll('#timeRangeGroup .btn-pill').forEach(b => {
    b.classList.toggle('active', parseInt(b.dataset.range) === activeRange);
  });
  fetchData();
}

// ============================================================
// CAMERA STREAM - always-on SSE
// ============================================================
function startCameraStream() {
  if (cameraStream) {
    console.log('[camera] stream already active');
    return;
  }
  console.log('[camera] opening SSE connection to /stream');
  setCameraStatus('neutral', 'Connecting');
  lastFrameRender = 0;

  cameraStream = new EventSource('/stream');

  cameraStream.onopen = () => {
    console.log('[camera] SSE connected, waiting for first frame');
    setCameraStatus('neutral', 'Stream connected — waiting for frame');
  };

  cameraStream.onmessage = (event) => {
    try {
      const data = JSON.parse(event.data);
      const now = Date.now();
      const isAlert = data.alert === true || data.alert === 'DANGER';
      // throttle non-alert frames; alerts render immediately
      if (!isAlert && (now - lastFrameRender) < FRAME_INTERVAL_MS) return;
      lastFrameRender = now;
      renderCameraFrame(data, isAlert);
    } catch (err) {
      console.error('[camera] SSE parse error:', err, 'raw data:', event.data?.substring(0, 100));
    }
  };

  cameraStream.onerror = (err) => {
    console.error('[camera] SSE error (browser will retry):', err);
    setCameraStatus('error', 'Stream interrupted — retrying');
  };
}

function stopCameraStream() {
  if (cameraStream) {
    console.log('[camera] closing SSE');
    cameraStream.close();
    cameraStream = null;
  }
}

function renderCameraFrame(data, isAlert) {
  const img = document.getElementById('cameraImage');
  const ph  = document.getElementById('cameraPlaceholder');
  if (data.img) {
    img.src = 'data:image/jpeg;base64,' + data.img;
    img.style.display = 'block';
    ph.style.display = 'none';
  }
  if (isAlert) {
    setCameraStatus('danger', 'Person detected in danger zone');
  } else {
    setCameraStatus('clear', 'Clear');
  }
  if (data.ts) {
    document.getElementById('cameraTimestamp').textContent =
      new Date(data.ts).toLocaleTimeString();
  }
}

function setCameraStatus(level, text) {
  const el = document.getElementById('cameraStatus');
  el.className = 'cam-status ' + (level || 'neutral');
  el.textContent = text;
}

// ============================================================
// LOCATION POLLING
// ============================================================
async function fetchZone() {
  try {
    const res = await fetch('/api/zone', { cache: 'no-store' });
    const data = await res.json();
    renderZone(data);
  } catch (err) {
    console.error('Zone fetch error:', err);
    document.getElementById('locationStatus').textContent = 'Error';
    document.getElementById('locationStatus').className = 'cam-status error';
  }
}

function renderZone(data) {
  const badge   = document.getElementById('zoneBadge');
  const sub     = document.getElementById('zoneSub');
  const status  = document.getElementById('locationStatus');
  const rssiEl  = document.getElementById('zoneRssiVal');
  const minorEl = document.getElementById('zoneMinorVal');
  const qualEl  = document.getElementById('zoneQualityVal');
  const tsEl    = document.getElementById('zoneUpdatedVal');

  if (data.zone === 'router offline' || data.stale) {
    badge.textContent = 'Router offline';
    badge.style.color = 'var(--accent-red)';
    sub.textContent = 'no data received in 30s';
    status.textContent = 'Offline';
    status.className = 'cam-status error';
    rssiEl.textContent = '--'; minorEl.textContent = '--';
    qualEl.textContent = '--'; tsEl.textContent = '--';
  } else if (data.zone === 'out of reach') {
    badge.textContent = 'Out of reach';
    badge.style.color = 'var(--accent-orange)';
    sub.textContent = 'no beacons in range';
    status.textContent = 'Searching';
    status.className = 'cam-status neutral';
    rssiEl.textContent = '--'; minorEl.textContent = '--';
    qualEl.textContent = '--';
    if (data.ts) tsEl.textContent = new Date(data.ts).toLocaleTimeString();
  } else if (data.zone) {
    const q = rssiToQuality(data.rssi);
    badge.textContent = data.zone;
    badge.style.color = q.color;
    sub.textContent = `Beacon Minor 0x${(data.minor || 0).toString(16).padStart(4, '0').toUpperCase()}`;
    status.textContent = 'Live';
    status.className = 'cam-status clear';
    rssiEl.textContent  = data.rssi !== null ? `${data.rssi} dBm` : '--';
    minorEl.textContent = `0x${(data.minor || 0).toString(16).padStart(4, '0').toUpperCase()}`;
    qualEl.textContent  = q.label;
    qualEl.style.color  = q.color;
    if (data.ts) tsEl.textContent = new Date(data.ts).toLocaleTimeString();
  } else {
    badge.textContent = 'Waiting';
    badge.style.color = 'var(--text-muted)';
    sub.textContent = 'no data yet';
    status.textContent = 'No data';
    status.className = 'cam-status neutral';
    rssiEl.textContent = '--'; minorEl.textContent = '--';
    qualEl.textContent = '--'; tsEl.textContent = '--';
  }
}

function rssiToQuality(rssi) {
  if (rssi === null || rssi === undefined) return { label: '--', color: 'var(--text-muted)' };
  if (rssi >= -65) return { label: 'Strong', color: 'var(--accent-green)' };
  if (rssi >= -80) return { label: 'Good',   color: 'var(--accent)' };
  if (rssi >= -90) return { label: 'Weak',   color: 'var(--accent-orange)' };
  return { label: 'Poor', color: 'var(--accent-red)' };
}

function startLocationPolling() {
  fetchZone();
  if (locationPoll) clearInterval(locationPoll);
  locationPoll = setInterval(fetchZone, 3000);
}

function stopLocationPolling() {
  if (locationPoll) { clearInterval(locationPoll); locationPoll = null; }
}

// ============================================================
// TAB SWITCHING
// ============================================================
function activateTab(tab) {
  document.querySelectorAll('.tab-btn').forEach(b => {
    b.classList.toggle('active', b.dataset.tab === tab);
  });
  document.getElementById('dashboard-panel').style.display = (tab === 'dashboard') ? '' : 'none';
  document.getElementById('camera-panel').style.display    = (tab === 'camera')    ? '' : 'none';
  document.getElementById('location-panel').style.display  = (tab === 'location')  ? '' : 'none';

  // Lifecycle: stream only when relevant
  if (tab === 'camera')   startCameraStream();   else stopCameraStream();
  if (tab === 'location') startLocationPolling(); else stopLocationPolling();
}

// ============================================================
// HELPERS
// ============================================================
function hexToRgba(hex, alpha) {
  const m = hex.replace('#', '').match(/.{2}/g);
  const [r, g, b] = m.map(x => parseInt(x, 16));
  return `rgba(${r}, ${g}, ${b}, ${alpha})`;
}

// ============================================================
// EVENT WIRING + BOOTSTRAP
// ============================================================
document.addEventListener('DOMContentLoaded', () => {
  initChart();

  // Metric cards
  document.querySelectorAll('.metric-card').forEach(c => {
    c.addEventListener('click', () => selectMetric(c.dataset.metric));
  });

  // Time range pills
  document.querySelectorAll('#timeRangeGroup .btn-pill').forEach(b => {
    b.addEventListener('click', () => selectRange(b.dataset.range));
  });

  // Tabs
  document.querySelectorAll('.tab-btn').forEach(b => {
    b.addEventListener('click', () => activateTab(b.dataset.tab));
  });

  // Initial fetch + interval
  fetchData();
  pollTimer = setInterval(fetchData, 5000);
});