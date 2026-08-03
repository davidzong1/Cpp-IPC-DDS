/**
 * dzplot — Time-series visualization frontend.
 *
 * Vanilla JS + Canvas 2D charts. No framework dependencies.
 * Communicates with dzplot.py backend via WebSocket.
 */

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
const state = {
  connected: false,
  socket: null,
  reconnectTimer: null,
  fps: 60,
  drawIntervalMs: 1000 / 60,
  topics: {},
  selectedFields: {},       // topic -> [field names]
  playback: { mode: "idle", source: "", speed: 1.0, loop: false },
  queueStats: {
    queue_size: 0, queue_capacity: 4096, fill_ratio: 0, zone: "normal",
    total_published: 0, total_dropped: 0, backpressure_active: false,
    backpressure_zone: "normal", keep_every_n: 1,
  },
  charts: {},               // topic_field -> Chart instance
  chartData: {},            // topic_field -> { times: [], values: [] }
  maxDataPoints: 600,       // 10s at 60fps
  sourceMode: "bag",        // "bag" | "live"
  // Render tracking — time-window actual frame counter
  renderFrameTimes: [],     // monotonic ms of each draw() call (rolling 1s window)
  drawFrameCount: 0,
};

// ---------------------------------------------------------------------------
// DOM refs
// ---------------------------------------------------------------------------
const $ = (id) => document.getElementById(id);

const dom = {
  connectionText: $("connectionText"),
  fpsSlider: $("fpsSlider"),
  fpsValue: $("fpsValue"),
  targetFps: $("targetFps"),
  actualFps: $("actualFps"),
  topicList: $("topicList"),
  topicCount: $("topicCount"),
  chartContainer: $("chartContainer"),
  chartPlaceholder: $("chartPlaceholder"),
  // Bag panel
  tabBag: $("tabBag"),
  tabLive: $("tabLive"),
  bagPanel: $("bagPanel"),
  livePanel: $("livePanel"),
  bagPath: $("bagPath"),
  bagSpeed: $("bagSpeed"),
  bagLoop: $("bagLoop"),
  loadBagBtn: $("loadBagBtn"),
  liveTopic: $("liveTopic"),
  liveMsgType: $("liveMsgType"),
  liveTransport: $("liveTransport"),
  liveDomain: $("liveDomain"),
  startSniffBtn: $("startSniffBtn"),
  // Playback
  btnStop: $("btnStop"),
  btnPause: $("btnPause"),
  btnPlay: $("btnPlay"),
  playbackInfo: $("playbackInfo"),
  // Queue status
  qSize: $("qSize"),
  qCap: $("qCap"),
  qWatermark: $("qWatermark"),
  qDropped: $("qDropped"),
  qBP: $("qBP"),
  qSub: $("qSub"),
  qTotalRx: $("qTotalRx"),
};

// ---------------------------------------------------------------------------
// Chart class — lightweight Canvas 2D time-series
// ---------------------------------------------------------------------------
class TimeSeriesChart {
  constructor(container, title) {
    this.container = container;
    this.title = title;
    this.data = [];           // [{time_ms, value}]
    this.maxPoints = state.maxDataPoints;
    this.yMin = null;        // auto-scale; null = auto
    this.yMax = null;
    this.color = this._randomColor();
    this.dirty = true;       // set when new data or resize arrives

    this.el = document.createElement("div");
    this.el.className = "chart-card";
    this.el.innerHTML = `
      <div class="chart-card-head">
        <span class="chart-title">${this.title}</span>
        <button class="chart-close icon-button" title="Remove">×</button>
      </div>
      <canvas class="chart-canvas"></canvas>
    `;
    this.canvas = this.el.querySelector(".chart-canvas");
    this.ctx = this.canvas.getContext("2d");

    this.el.querySelector(".chart-close").onclick = () => this.destroy();
    container.appendChild(this.el);

    this._resize();
    this._draw();
  }

  _randomColor() {
    const hues = [200, 340, 50, 120, 280, 30, 170, 310];
    const h = hues[Math.floor(Math.random() * hues.length)];
    return `hsl(${h}, 65%, 50%)`;
  }

  markDirty() { this.dirty = true; }

  push(timeMs, value) {
    this.data.push({ time_ms: timeMs, value });
    if (this.data.length > this.maxPoints) {
      this.data = this.data.slice(-this.maxPoints);
    }
    this.dirty = true;
  }

  pushBatch(points) {
    for (const p of points) {
      this.data.push({ time_ms: p.time_ms, value: p.value });
    }
    if (this.data.length > this.maxPoints) {
      this.data = this.data.slice(-this.maxPoints);
    }
    if (points.length > 0) this.dirty = true;
  }

  _resize() {
    const rect = this.el.getBoundingClientRect();
    const dpr = window.devicePixelRatio || 1;
    const w = rect.width - 16;
    const h = Math.max(120, Math.min(300, w * 0.45));
    const oldW = this._w, oldH = this._h;
    this.canvas.style.width = w + "px";
    this.canvas.style.height = h + "px";
    this.canvas.width = w * dpr;
    this.canvas.height = h * dpr;
    this.ctx.setTransform(1, 0, 0, 1, 0, 0);
    this.ctx.scale(dpr, dpr);
    this._w = w;
    this._h = h;
    if (oldW !== w || oldH !== h) this.dirty = true;
  }

  _draw() {
    this._resize();
    const ctx = this.ctx;
    const w = this._w;
    const h = this._h;
    if (!w || !h) return;

    // Clear
    ctx.clearRect(0, 0, w, h);

    // Background
    ctx.fillStyle = "#1a1d23";
    ctx.fillRect(0, 0, w, h);

    // Grid
    ctx.strokeStyle = "rgba(255,255,255,0.06)";
    ctx.lineWidth = 1;
    const gridLines = 5;
    for (let i = 0; i <= gridLines; i++) {
      const y = (h / gridLines) * i;
      ctx.beginPath();
      ctx.moveTo(0, y);
      ctx.lineTo(w, y);
      ctx.stroke();
    }
    for (let i = 0; i <= 4; i++) {
      const x = (w / 4) * i;
      ctx.beginPath();
      ctx.moveTo(x, 0);
      ctx.lineTo(x, h);
      ctx.stroke();
    }

    if (this.data.length < 2) {
      ctx.fillStyle = "rgba(255,255,255,0.3)";
      ctx.font = "12px monospace";
      ctx.textAlign = "center";
      ctx.fillText("Waiting for data...", w / 2, h / 2);
      return;
    }

    // Compute bounds
    let yMin = this.yMin !== null ? this.yMin : Infinity;
    let yMax = this.yMax !== null ? this.yMax : -Infinity;
    if (this.yMin === null || this.yMax === null) {
      for (const p of this.data) {
        if (p.value < yMin) yMin = p.value;
        if (p.value > yMax) yMax = p.value;
      }
      const pad = (yMax - yMin) * 0.1 || 1;
      yMin -= pad;
      yMax += pad;
    }

    const xMin = this.data[0].time_ms;
    const xMax = this.data[this.data.length - 1].time_ms;
    const xRange = xMax - xMin || 1;
    const yRange = yMax - yMin || 1;

    const margin = { top: 10, right: 10, bottom: 24, left: 50 };
    const pw = w - margin.left - margin.right;
    const ph = h - margin.top - margin.bottom;

    const tx = (v) => margin.left + ((v - xMin) / xRange) * pw;
    const ty = (v) => margin.top + (1 - (v - yMin) / yRange) * ph;

    // Y-axis labels
    ctx.fillStyle = "rgba(255,255,255,0.5)";
    ctx.font = "10px monospace";
    ctx.textAlign = "right";
    for (let i = 0; i <= 4; i++) {
      const val = yMin + (yRange / 4) * i;
      const label = Math.abs(val) < 0.01 ? "0" :
        Math.abs(val) > 1000 ? val.toExponential(1) :
        Math.abs(val) < 1 ? val.toFixed(3) : val.toFixed(1);
      ctx.fillText(label, margin.left - 4, ty(val) + 4);
    }

    // Line
    ctx.strokeStyle = this.color;
    ctx.lineWidth = 2;
    ctx.lineJoin = "round";
    ctx.beginPath();
    let firstPoint = true;
    for (const p of this.data) {
      const x = tx(p.time_ms);
      const y = ty(p.value);
      if (firstPoint) { ctx.moveTo(x, y); firstPoint = false; }
      else { ctx.lineTo(x, y); }
    }
    ctx.stroke();

    // Latest value indicator
    if (this.data.length > 0) {
      const last = this.data[this.data.length - 1];
      const lx = tx(last.time_ms);
      const ly = ty(last.value);
      ctx.fillStyle = this.color;
      ctx.beginPath();
      ctx.arc(lx, ly, 4, 0, Math.PI * 2);
      ctx.fill();
    }

    // X-axis label
    ctx.fillStyle = "rgba(255,255,255,0.4)";
    ctx.font = "10px monospace";
    ctx.textAlign = "center";
    ctx.fillText("time →", w / 2, h - 4);
  }

  destroy() {
    this.el.remove();
    this.dirty = false;
    return null;
  }
}

// ---------------------------------------------------------------------------
// Render loop — rAF-based, dirty-flag on-demand, FPS-throttled
// ---------------------------------------------------------------------------
let renderRafId = null;
let lastDrawTime = 0;

function startRenderLoop() {
  if (renderRafId !== null) return;

  function tick(now) {
    renderRafId = requestAnimationFrame(tick);

    const intervalMs = 1000 / Math.max(state.fps, 1);
    if (now - lastDrawTime < intervalMs) return;

    // Collect dirty charts that have real data
    const dirty = [];
    for (const key in state.charts) {
      const c = state.charts[key];
      if (c.dirty && c.data.length > 0) {
        dirty.push(c);
      }
    }
    if (dirty.length === 0) return;

    lastDrawTime = now;
    const frameStart = performance.now();
    for (const c of dirty) {
      c._draw();
      c.dirty = false;
    }

    // --- Time-window actual FPS ---
    const nowMs = performance.now();
    state.renderFrameTimes.push(nowMs);
    // Prune events older than 1 second
    const cutoff = nowMs - 1000;
    while (state.renderFrameTimes.length > 0 &&
           state.renderFrameTimes[0] < cutoff) {
      state.renderFrameTimes.shift();
    }
    state.drawFrameCount = state.renderFrameTimes.length;
    dom.actualFps.textContent = String(state.drawFrameCount);
  }

  lastDrawTime = performance.now();
  renderRafId = requestAnimationFrame(tick);
}

function stopRenderLoop() {
  if (renderRafId !== null) {
    cancelAnimationFrame(renderRafId);
    renderRafId = null;
  }
  state.renderFrameTimes = [];
  state.drawFrameCount = 0;
  dom.actualFps.textContent = "0";
}

/**
 * Called when FPS changes via slider — the next rAF tick immediately
 * picks up the new state.fps / state.drawIntervalMs because tick()
 * reads them live.  We only reset the draw throttle so the user sees
 * the effect instantly (no waiting for the old interval to expire).
 */
function onFpsChanged() {
  state.drawIntervalMs = 1000 / Math.max(state.fps, 1);
  lastDrawTime = 0;  // force next rAF tick to draw regardless of elapsed
}

// ---------------------------------------------------------------------------
// WebSocket
// ---------------------------------------------------------------------------
function connect() {
  const protocol = location.protocol === "https:" ? "wss:" : "ws:";
  const wsUrl = `${protocol}//${location.host}/ws`;

  const ws = new WebSocket(wsUrl);
  state.socket = ws;

  ws.onopen = () => {
    state.connected = true;
    dom.connectionText.textContent = "Connected";
    dom.connectionText.style.color = "#4caf50";
    startRenderLoop();
  };

  ws.onmessage = (e) => {
    try {
      const msg = JSON.parse(e.data);
      handleMessage(msg);
    } catch (err) {
      console.warn("dzplot: bad message", err);
    }
  };

  ws.onclose = () => {
    state.connected = false;
    dom.connectionText.textContent = "Reconnecting...";
    dom.connectionText.style.color = "#ff9800";
    scheduleReconnect();
  };

  ws.onerror = () => {
    ws.close();
  };
}

function scheduleReconnect() {
  if (state.reconnectTimer) return;
  state.reconnectTimer = setTimeout(() => {
    state.reconnectTimer = null;
    connect();
  }, 2000);
}

function sendCommand(cmd) {
  if (state.socket && state.socket.readyState === WebSocket.OPEN) {
    state.socket.send(JSON.stringify(cmd));
  }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/**
 * Walk a dotted path through a nested object.
 *   getNested({a: {b: 3}}, "a.b") → 3
 *   getNested({a: [10,20]}, "a.1") → 20
 *   getNested({x: 1}, "y") → undefined
 */
function getNested(obj, path) {
  if (obj == null || typeof obj !== "object") return undefined;
  const parts = String(path).split(".");
  let cur = obj;
  for (const key of parts) {
    if (cur == null || typeof cur !== "object") return undefined;
    if (Array.isArray(cur)) {
      const idx = Number(key);
      if (!Number.isInteger(idx) || idx < 0 || idx >= cur.length) return undefined;
      cur = cur[idx];
    } else {
      if (!(key in cur)) return undefined;
      cur = cur[key];
    }
  }
  return cur;
}

/**
 * Convert a sample's timestamp_ns (nanoseconds since epoch) to
 * milliseconds for chart X axis.  Validates the result is within
 * ±1 year of now; falls back to the frame-level timestamp_ms or
 * Date.now() if the per-sample timestamp is missing or unreasonable.
 */
const ONE_YEAR_MS = 365 * 24 * 3600 * 1000;
function sampleTimeMs(sample, fallbackMs) {
  if (sample && typeof sample.timestamp_ns === "number") {
    const ms = sample.timestamp_ns / 1e6;
    const now = Date.now();
    if (ms > now - ONE_YEAR_MS && ms < now + ONE_YEAR_MS) {
      return ms;
    }
  }
  return fallbackMs || Date.now();
}

// ---------------------------------------------------------------------------
// Message handler
// ---------------------------------------------------------------------------
function handleMessage(msg) {
  switch (msg.kind) {
    case "hello":
      // Initialize topics
      for (const t of msg.topics || []) {
        state.topics[t.topic] = t;
      }
      if (msg.playback) state.playback = msg.playback;
      if (msg.fps) {
        state.fps = msg.fps;
        dom.fpsSlider.value = msg.fps;
        dom.fpsValue.textContent = msg.fps;
        dom.targetFps.textContent = msg.fps;
      }
      updateTopicList();
      updatePlaybackUI();
      break;

    case "frame":
      handleFrame(msg);
      break;

    case "ack":
      if (!msg.ok) {
        console.warn("dzplot: command failed", msg.error || msg);
        alert("Error: " + (msg.error || "unknown"));
      }
      if (msg.topics) {
        // Refresh topics after load/sniff — ensure safe defaults
        for (const t of msg.topics) {
          state.topics[t.topic] = {
            topic: t.topic,
            msg_type: t.msg_type ?? state.topics[t.topic]?.msg_type ?? "",
            source: t.source ?? state.topics[t.topic]?.source ?? "",
            active: t.active ?? state.topics[t.topic]?.active ?? true,
            sample_count: t.sample_count ?? state.topics[t.topic]?.sample_count ?? 0,
          };
        }
        updateTopicList();
      }
      if (msg.fps !== undefined) {
        state.fps = msg.fps;
        dom.fpsValue.textContent = msg.fps;
        dom.targetFps.textContent = msg.fps;
        dom.fpsSlider.value = msg.fps;
        restartRenderLoop();
      }
      if (msg.playback) {
        state.playback = { ...state.playback, ...msg.playback };
        updatePlaybackUI();
      }
      if (msg.mode) {
        state.playback.mode = msg.mode;
        updatePlaybackUI();
      }
      break;

    default:
      break;
  }
}

function handleFrame(frame) {
  const { batch, stats, timestamp_ms } = frame;

  // Update queue stats
  if (stats) {
    state.queueStats = { ...state.queueStats, ...stats };
    updateQueueStats();
  }

  if (!batch || batch.length === 0) return;

  // Group samples by topic
  const byTopic = {};
  for (const sample of batch) {
    const topic = sample.topic;
    if (!byTopic[topic]) byTopic[topic] = [];
    byTopic[topic].push(sample);
  }

  // Process each topic
  for (const [topic, samples] of Object.entries(byTopic)) {
    // Track topic in state
    if (!state.topics[topic]) {
      state.topics[topic] = {
        topic,
        msg_type: samples[0].msg_type || "",
        source: samples[0].source || "",
        active: true,
        sample_count: 0,
      };
      updateTopicList();
    }
    state.topics[topic].sample_count += samples.length;

    // Inline-update sample count in sidebar DOM (avoid full topicList rebuild per frame)
    for (const el of document.querySelectorAll(".topic-item")) {
      if (el.dataset.topic === topic) {
        const countSpan = el.querySelector(".topic-sample-count");
        if (countSpan) {
          countSpan.textContent = `${state.topics[topic].sample_count} samples`;
        }
        break;
      }
    }

    // Get selected fields for this topic
    const fields = state.selectedFields[topic];
    if (!fields || fields.length === 0) continue;

    // Frame timestamp as fallback for samples without their own
    const fallbackTimeMs = timestamp_ms || Date.now();

    // Extract field values from each sample, build batch points per chart
    const chartPoints = {};  // chartKey -> [{time_ms, value}]
    for (const fieldName of fields) {
      const chartKey = `${topic}\x00${fieldName}`;
      if (!chartPoints[chartKey]) chartPoints[chartKey] = [];

      for (const sample of samples) {
        // Per-sample timestamp (ns → ms), validated; falls back to frame ts
        const tMs = sampleTimeMs(sample, fallbackTimeMs);

        let value = null;

        if (sample.fields && typeof sample.fields === "object") {
          const raw = getNested(sample.fields, fieldName);
          if (raw !== undefined) {
            value = parseFloat(raw);
          }
        }

        // Explicit data_length field (shipped by dzplot backend)
        if (value === null && fieldName === "data_length" &&
            typeof sample.data_length === "number") {
          value = sample.data_length;
        }

        if (value !== null && !isNaN(value) && isFinite(value)) {
          if (!state.chartData[chartKey]) {
            state.chartData[chartKey] = { times: [], values: [] };
          }
          state.chartData[chartKey].times.push(tMs);
          state.chartData[chartKey].values.push(value);

          chartPoints[chartKey].push({ time_ms: tMs, value });
        }
      }
    }

    // Push batch points to charts (single DOM update per chart per frame)
    for (const [chartKey, points] of Object.entries(chartPoints)) {
      if (points.length === 0) continue;

      // Trim chartData
      const cd = state.chartData[chartKey];
      while (cd.times.length > state.maxDataPoints) {
        cd.times.shift();
        cd.values.shift();
      }

      // Update chart (use pushBatch for batched insertion)
      if (state.charts[chartKey]) {
        state.charts[chartKey].pushBatch(points);
      }
    }
  }
}

// ---------------------------------------------------------------------------
// UI updates
// ---------------------------------------------------------------------------
function updateTopicList() {
  const topics = Object.values(state.topics);
  dom.topicCount.textContent = topics.length;

  dom.topicList.innerHTML = topics.map(t => {
    const fields = state.selectedFields[t.topic] || [];
    const isSelected = fields.length > 0;
    return `
      <div class="topic-item ${isSelected ? "active" : ""}" data-topic="${t.topic}">
        <div class="topic-item-head">
          <span class="topic-name" title="${t.topic}">${t.topic}</span>
          <span class="topic-source ${t.source || "?"}">${t.source || "?"}</span>
        </div>
        <div class="topic-item-meta">
          <span>${t.msg_type || "?"}</span>
          <span class="topic-sample-count">${t.sample_count ?? 0} samples</span>
        </div>
        <div class="topic-item-fields">
          ${renderFieldInputs(t.topic, fields, t)}
        </div>
      </div>
    `;
  }).join("");

  // Bind field input events
  dom.topicList.querySelectorAll(".field-add-btn").forEach(btn => {
    btn.onclick = () => {
      const topic = btn.dataset.topic;
      const input = dom.topicList.querySelector(`.field-input[data-topic="${topic}"]`);
      const fieldName = input.value.trim();
      if (!fieldName) return;
      addField(topic, fieldName);
      input.value = "";
    };
  });

  dom.topicList.querySelectorAll(".field-remove-btn").forEach(btn => {
    btn.onclick = () => {
      removeField(btn.dataset.topic, btn.dataset.field);
    };
  });

  dom.topicList.querySelectorAll(".quick-field-btn").forEach(btn => {
    btn.onclick = () => {
      addField(btn.dataset.topic, btn.dataset.field);
    };
  });
}

function renderFieldInputs(topic, fields, meta) {
  // Common fields that might be in the message
  const commonFields = ["data", "x", "y", "z", "timestamp", "data_length",
    "header.seq", "header.stamp", "position.x", "position.y", "position.z"];

  let html = '<div class="field-tags">';
  for (const f of fields) {
    html += `<span class="field-tag">
      ${f}
      <button class="field-remove-btn" data-topic="${topic}" data-field="${f}">×</button>
    </span>`;
  }
  html += '</div>';

  html += `<div class="field-input-row">
    <input class="field-input" data-topic="${topic}" type="text" placeholder="field name..." />
    <button class="field-add-btn primary-action small" data-topic="${topic}">+</button>
  </div>`;

  html += '<div class="quick-fields">';
  for (const f of commonFields.slice(0, 6)) {
    html += `<button class="quick-field-btn" data-topic="${topic}" data-field="${f}">${f}</button>`;
  }
  html += '</div>';

  return html;
}

function addField(topic, fieldName) {
  if (!state.selectedFields[topic]) {
    state.selectedFields[topic] = [];
  }
  if (state.selectedFields[topic].includes(fieldName)) return;

  state.selectedFields[topic] = [...state.selectedFields[topic], fieldName];
  updateTopicList();

  // Create chart
  const chartKey = `${topic}\x00${fieldName}`;
  if (!state.charts[chartKey]) {
    dom.chartPlaceholder.style.display = "none";
    state.charts[chartKey] = new TimeSeriesChart(
      dom.chartContainer,
      `${topic} / ${fieldName}`
    );
  }

  // Notify server
  sendCommand({
    action: "select_fields",
    topic,
    fields: state.selectedFields[topic],
  });
}

function removeField(topic, fieldName) {
  if (!state.selectedFields[topic]) return;
  state.selectedFields[topic] = state.selectedFields[topic].filter(f => f !== fieldName);
  updateTopicList();

  const chartKey = `${topic}\x00${fieldName}`;
  if (state.charts[chartKey]) {
    state.charts[chartKey].destroy();
    delete state.charts[chartKey];
    delete state.chartData[chartKey];
  }

  if (Object.keys(state.charts).length === 0) {
    dom.chartPlaceholder.style.display = "flex";
  }

  sendCommand({
    action: "select_fields",
    topic,
    fields: state.selectedFields[topic],
  });
}

function updatePlaybackUI() {
  const { mode, source, speed, loop } = state.playback;
  dom.playbackInfo.textContent = mode === "playing"
    ? `${source} ${speed}x${loop ? " 🔁" : ""}`
    : mode === "paused" ? "Paused" : "Idle";
}

function updateQueueStats() {
  const s = state.queueStats;
  dom.qSize.textContent = s.queue_size || 0;
  dom.qCap.textContent = s.queue_capacity || 4096;
  dom.qWatermark.textContent = Math.round((s.fill_ratio || 0) * 100) + "%";
  dom.qDropped.textContent = s.total_dropped || 0;
  dom.qBP.textContent = s.backpressure_active ? "ON ⚠" : "off";
  dom.qSub.textContent = (s.keep_every_n || 1) + "x";
  dom.qTotalRx.textContent = s.total_published || 0;

  // Color coding
  const zone = s.zone || "normal";
  dom.qWatermark.style.color = zone === "emergency" ? "#f44336" :
    zone === "heavy" ? "#ff9800" : zone === "warn" ? "#ffc107" : "#4caf50";
  dom.qBP.style.color = s.backpressure_active ? "#ff9800" : "#4caf50";
}

function restartRenderLoop() {
  if (renderTimer) { clearInterval(renderTimer); renderTimer = null; }
  startRenderLoop();
}

// ---------------------------------------------------------------------------
// Event bindings
// ---------------------------------------------------------------------------

// FPS slider — live reconfig, no timer restart needed
dom.fpsSlider.oninput = () => {
  const fps = parseInt(dom.fpsSlider.value);
  dom.fpsValue.textContent = fps;
};
dom.fpsSlider.onchange = () => {
  const fps = parseInt(dom.fpsSlider.value);
  state.fps = fps;
  dom.targetFps.textContent = fps;
  onFpsChanged();
  sendCommand({ action: "set_fps", fps });
};

// Source tabs
dom.tabBag.onclick = () => {
  state.sourceMode = "bag";
  dom.tabBag.classList.add("active");
  dom.tabLive.classList.remove("active");
  dom.bagPanel.hidden = false;
  dom.livePanel.hidden = true;
};
dom.tabLive.onclick = () => {
  state.sourceMode = "live";
  dom.tabLive.classList.add("active");
  dom.tabBag.classList.remove("active");
  dom.bagPanel.hidden = true;
  dom.livePanel.hidden = false;
};

// Bag load
dom.loadBagBtn.onclick = () => {
  const path = dom.bagPath.value.trim();
  if (!path) { alert("Enter a .bag file path"); return; }
  const speed = parseFloat(dom.bagSpeed.value) || 1.0;
  const loop = dom.bagLoop.checked;
  sendCommand({ action: "load_bag", path, speed, loop });
};

// Sniff start
dom.startSniffBtn.onclick = () => {
  const topic = dom.liveTopic.value.trim();
  if (!topic) { alert("Enter a topic name"); return; }
  const msgType = dom.liveMsgType.value.trim() || "StdRawMessage";
  const transport = dom.liveTransport.value;
  const domain = parseInt(dom.liveDomain.value) || 0;
  sendCommand({
    action: "start_sniff",
    topics: [{ topic, msg_type: msgType, transport, domain }],
    transport,
    domain,
  });
};

// Playback controls
dom.btnStop.onclick = () => sendCommand({ action: "stop" });
dom.btnPause.onclick = () => sendCommand({ action: "pause" });
dom.btnPlay.onclick = () => sendCommand({ action: "resume" });

// Status panel collapse
document.querySelector(".status-panel .collapse-toggle")?.addEventListener("click", function() {
  const panel = this.closest(".status-panel");
  panel.classList.toggle("collapsed");
  this.textContent = panel.classList.contains("collapsed") ? "+" : "−";
});

// Window resize — redraw charts
window.addEventListener("resize", () => {
  for (const key in state.charts) {
    state.charts[key]._draw();
  }
});

// Periodic status polling (every 2s) for queue stats and playback state
let statusPollTimer = null;
function startStatusPolling() {
  if (statusPollTimer) return;
  statusPollTimer = setInterval(() => {
    if (state.connected) {
      sendCommand({ action: "get_status" });
    }
  }, 2000);
}
function stopStatusPolling() {
  if (statusPollTimer) { clearInterval(statusPollTimer); statusPollTimer = null; }
}

// Update the handleMessage ack handler to also process get_status responses
const origHandleMessage = handleMessage;
handleMessage = function(msg) {
  origHandleMessage(msg);
  // get_status polls update queue stats and playback without changing UI mode
  if (msg.kind === "ack" && msg.ok && msg.queue_stats) {
    state.queueStats = { ...state.queueStats, ...msg.queue_stats };
    updateQueueStats();
  }
  if (msg.kind === "ack" && msg.ok && msg.playback) {
    state.playback = { ...state.playback, ...msg.playback };
    updatePlaybackUI();
  }
};

// Hook into connect/disconnect for status polling
const origConnect = connect;
connect = function() {
  origConnect();
  startStatusPolling();
};
const origScheduleReconnect = scheduleReconnect;
scheduleReconnect = function() {
  stopStatusPolling();
  origScheduleReconnect();
};

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
connect();
