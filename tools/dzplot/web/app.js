/**
 * dzplot — Time-series visualization frontend.
 *
 * Vanilla JS + Canvas 2D charts. No framework dependencies.
 * Communicates with main.py backend via WebSocket (2026-09-13 由 dzplot.py 改名).
 */

import { createWorkspace } from "./workspace.js";
import { createPlotWindows, seriesKey } from "./plots.js";

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
const state = {
  connected: false,
  socket: null,
  reconnectTimer: null,
  fps: 60,
  drawIntervalMs: 1000 / 60,
  topics: Object.create(null),
  selectedFields: Object.create(null),
  availableFields: Object.create(null),
  latestFields: Object.create(null),
  collapsedTopics: Object.create(null),
  playback: { mode: "idle", source: "", speed: 1.0, loop: false },
  queueStats: {
    queue_size: 0, queue_capacity: 4096, fill_ratio: 0, zone: "normal",
    total_published: 0, total_dropped: 0, backpressure_active: false,
    backpressure_zone: "normal", keep_every_n: 1,
  },
  charts: Object.create(null),
  chartData: Object.create(null),
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
// Render loop — rAF-based, dirty-flag on-demand, FPS-throttled
// ---------------------------------------------------------------------------
let renderRafId = null;
let lastDrawTime = 0;

function startRenderLoop() {
  if (renderRafId !== null) return;

  function tick(now) {
    renderRafId = requestAnimationFrame(tick);

    if (document.hidden || workspace.view === "viz") { dom.actualFps.textContent = "0"; return; }
    const intervalMs = 1000 / Math.max(state.fps, 1);
    if (now - lastDrawTime < intervalMs) return;

    // Collect dirty charts that have real data
    const dirty = [];
    for (const key in state.charts) {
      const c = state.charts[key];
      if (c.dirty) {
        dirty.push(c);
      }
    }
    if (dirty.length === 0) {
      if (now - lastDrawTime > 1000) dom.actualFps.textContent = "0";
      return;
    }

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
    dom.connectionText.textContent = "已连接";
    $("connectionDot").style.background = "#589b69";
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
    dom.connectionText.textContent = "正在重连…";
    $("connectionDot").style.background = "#d69c39";
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
    return true;
  }
  notify("连接尚未建立，请稍后重试。");
  return false;
}

let notificationTimer;
function notify(message) {
  $("notification").textContent = message;
  $("notification").hidden = false;
  clearTimeout(notificationTimer);
  notificationTimer = setTimeout(() => { $("notification").hidden = true; }, 5000);
}

function escapeHtml(value) {
  return String(value).replaceAll("&", "&amp;").replaceAll("<", "&lt;").replaceAll(">", "&gt;").replaceAll('"', "&quot;").replaceAll("'", "&#39;");
}

function numericFields(value, prefix = "", result = [], depth = 0) {
  if (depth > 7 || result.length >= 160) return result;
  if (typeof value === "number" && Number.isFinite(value) && prefix) result.push(prefix);
  else if (value && typeof value === "object") {
    const entries = Object.entries(value);
    for (const [key, item] of Array.isArray(value) ? entries.slice(0, 12) : entries) {
      numericFields(item, prefix ? `${prefix}.${key}` : key, result, depth + 1);
      if (result.length >= 160) break;
    }
  }
  return result;
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
    if (Number.isFinite(ms) && ms > 0) {
      return ms;
    }
  }
  return fallbackMs || Date.now();
}

// ---------------------------------------------------------------------------
// Message handler
// ---------------------------------------------------------------------------
let selectionInitialized = false;
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
      // 首次打开时兼容老版本的全局选择；已保存的空窗口及重连均以本地选择为准。
      if (!selectionInitialized) {
        selectionInitialized = true;
        if (!plots.restored) for (const [topic, fields] of Object.entries(msg.selected_fields || {})) plots.addFields(topic, fields);
      }
      for (const topic of new Set([...Object.keys(msg.selected_fields || {}), ...Object.keys(state.selectedFields)])) {
        sendCommand({ action: "select_fields", topic, fields: state.selectedFields[topic] || [] });
      }
      updatePlaybackUI();
      break;

    case "frame":
      handleFrame(msg);
      break;

    case "ack":
      if (!msg.ok) {
        console.warn("dzplot: command failed", msg.error || msg);
        notify("操作失败：" + (msg.error || "未知错误"));
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
      if (msg.ok && ["load_bag", "start_sniff"].includes(msg.action)) { workspace.closeSource(); workspace.resetTime(); }
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
  const byTopic = Object.create(null);
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
    }
    state.topics[topic].msg_type = samples.at(-1).msg_type || state.topics[topic].msg_type;
    state.topics[topic].sample_count += samples.length;
    const latest = samples.at(-1);
    state.latestFields[topic] = { ...latest.fields };
    if (typeof latest.data_length === "number") state.latestFields[topic].data_length = latest.data_length;
    const discovered = [...new Set([...(state.availableFields[topic] || []), ...numericFields(state.latestFields[topic])])];
    const changed = discovered.length !== (state.availableFields[topic]?.length || 0);
    state.availableFields[topic] = discovered;
    if (changed || ![...dom.topicList.children].some((item) => item.dataset.topic === topic)) updateTopicList();
    workspace.sampleTime(sampleTimeMs(latest, timestamp_ms));

    // Inline-update sample count in sidebar DOM (avoid full topicList rebuild per frame)
    for (const el of document.querySelectorAll(".topic-item")) {
      if (el.dataset.topic === topic) {
        const countSpan = el.querySelector(".topic-sample-count");
        if (countSpan) {
          countSpan.textContent = `${state.topics[topic].sample_count} 个样本`;
        }
        for (const row of el.querySelectorAll(".field-row")) {
          const value = getNested(state.latestFields[topic], row.dataset.field);
          row.querySelector(".field-value").textContent = typeof value === "number" ? Number(value.toPrecision(6)).toString() : "—";
        }
        break;
      }
    }

    // Get selected fields for this topic
    const fields = state.selectedFields[topic];
    if (!fields || fields.length === 0) continue;

    // Frame timestamp as fallback for samples without their own
    const fallbackTimeMs = timestamp_ms || Date.now();

    // 同一字段在多个窗口中显示时仅写入一次共享缓冲。
    const updated = new Set();
    for (const fieldName of fields) {
      const chartKey = seriesKey(topic, fieldName);

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

          updated.add(chartKey);
        }
      }
    }

    for (const chartKey of updated) {
      const cd = state.chartData[chartKey];
      while (cd.times.length > state.maxDataPoints) {
        cd.times.shift();
        cd.values.shift();
      }

      for (const chart of Object.values(state.charts)) if (chart.series.has(chartKey)) chart.markDirty();
    }
  }
}

// ---------------------------------------------------------------------------
// UI updates
// ---------------------------------------------------------------------------
function updateTopicList() {
  const topics = Object.values(state.topics);
  const focused = document.activeElement;
  const draft = focused?.classList.contains("field-input") ? { topic: focused.dataset.topic, value: focused.value, position: focused.selectionStart } : null;
  const scrollTop = dom.topicList.scrollTop;
  dom.topicCount.textContent = topics.length;
  $("datasetEmpty").hidden = topics.length > 0;
  const selected = plots.selectedFields();
  dom.topicList.innerHTML = topics.map((t) => {
    const topic = escapeHtml(t.topic);
    const fields = selected[t.topic] || [];
    const available = [...new Set([...(state.availableFields[t.topic] || []), ...(state.selectedFields[t.topic] || [])])];
    const source = { bag: "回放", live: "实时", visualizer: "3D" }[t.source] || "数据";
    return `
      <div class="topic-item ${fields.length ? "active" : ""}" data-topic="${topic}">
        <div class="topic-item-head">
          <button class="topic-expand" type="button" aria-label="展开或收起 ${topic}">${state.collapsedTopics[t.topic] ? "▸" : "▾"}</button>
          <input class="topic-select" type="checkbox" aria-label="将 ${topic} 的全部数值字段显示在当前窗口" ${available.length && available.every(field => fields.includes(field)) ? "checked" : ""} ${available.length ? "" : "disabled"}/>
          <span class="topic-name" draggable="true" title="${topic} · 拖到绘图窗口添加全部数值字段">${topic}</span>
          <span class="topic-source">${source}</span>
          <button class="topic-viz-btn" type="button" title="将话题添加到 3D 视图" aria-label="将 ${topic} 添加到 3D 视图">⬡</button>
        </div>
        <div class="topic-item-meta"><span>${escapeHtml(t.msg_type || "等待消息类型")}</span><span class="topic-sample-count">${t.sample_count ?? 0} 个样本</span></div>
        <div class="topic-item-fields">
          ${available.map((field) => {
            const value = getNested(state.latestFields[t.topic], field);
            return `<label class="field-row ${fields.includes(field) ? "selected" : ""}" draggable="true" data-field="${escapeHtml(field)}">
              <input type="checkbox" ${fields.includes(field) ? "checked" : ""}/><span class="field-name" title="${escapeHtml(field)}">${escapeHtml(field)}</span><span class="field-value">${typeof value === "number" ? Number(value.toPrecision(6)) : "—"}</span>
            </label>`;
          }).join("")}
          <div class="field-input-row"><input class="field-input" data-topic="${topic}" type="text" placeholder="手动添加字段，如 position.x" aria-label="${topic} 的字段路径"/><button class="field-add-btn primary-action small" type="button" aria-label="添加字段">+</button></div>
        </div>
      </div>`;
  }).join("");
  for (const item of dom.topicList.children) {
    const topic = item.dataset.topic;
    item.querySelector(".topic-expand").onclick = () => {
      state.collapsedTopics[topic] = !state.collapsedTopics[topic];
      item.querySelector(".topic-expand").textContent = state.collapsedTopics[topic] ? "▸" : "▾";
      workspace.filter();
    };
    item.querySelector(".topic-viz-btn").onclick = () => workspace.prepareDisplay(topic, state.topics[topic].msg_type);
    const topicSelect = item.querySelector(".topic-select");
    topicSelect.indeterminate = Boolean(selected[topic]?.length) && !topicSelect.checked;
    topicSelect.onchange = () => topicSelect.checked ? addTopic(topic) : plots.removeTopic(topic);
    item.querySelector(".topic-name").ondragstart = (event) => {
      event.dataTransfer.setData("application/dzplot-topic", JSON.stringify({ topic }));
      event.dataTransfer.effectAllowed = "copy";
    };
    const input = item.querySelector(".field-input");
    const add = () => { const field = input.value.trim(); if (field) addField(topic, field); };
    item.querySelector(".field-add-btn").onclick = add;
    input.onkeydown = (event) => { if (event.key === "Enter") add(); };
    for (const row of item.querySelectorAll(".field-row")) {
      row.querySelector("input").onchange = (event) => event.target.checked ? addField(topic, row.dataset.field) : removeField(topic, row.dataset.field);
      row.ondragstart = (event) => { event.dataTransfer.setData("application/dzplot-field", JSON.stringify({ topic, field: row.dataset.field })); event.dataTransfer.effectAllowed = "copy"; };
    }
  }
  workspace.filter();
  dom.topicList.scrollTop = scrollTop;
  if (draft) {
    const input = [...dom.topicList.querySelectorAll(".field-input")].find((item) => item.dataset.topic === draft.topic);
    if (input) { input.value = draft.value; input.focus(); input.setSelectionRange(draft.position, draft.position); }
  }
}

function syncPlotSelection() {
  const previous = state.selectedFields;
  state.selectedFields = plots.selectedFields(null);
  const keys = new Set(Object.entries(state.selectedFields).flatMap(([topic, fields]) => fields.map(field => seriesKey(topic, field))));
  for (const key of Object.keys(state.chartData)) if (!keys.has(key)) delete state.chartData[key];
  if (state.connected) for (const topic of new Set([...Object.keys(previous), ...Object.keys(state.selectedFields)])) {
    const fields = state.selectedFields[topic] || [];
    if (JSON.stringify(previous[topic] || []) !== JSON.stringify(fields)) sendCommand({ action: "select_fields", topic, fields });
  }
  updateTopicList();
  workspace.updateSeries();
}

function addField(topic, fieldName, id = state.activePlotId) {
  plots.addFields(topic, [fieldName], id);
}

function addTopic(topic, id = state.activePlotId) {
  const fields = [...new Set([...(state.availableFields[topic] || []), ...(state.selectedFields[topic] || [])])];
  if (!fields.length) { notify("该话题尚未发现数值字段，请等待数据或手动输入字段路径。"); return; }
  plots.addFields(topic, fields, id);
}

function removeField(topic, fieldName, id = state.activePlotId) {
  plots.removeField(topic, fieldName, id);
}

function updatePlaybackUI() {
  const { mode, source, speed, loop } = state.playback;
  dom.playbackInfo.textContent = mode === "playing"
    ? source === "bag" ? `回放 ${speed || 1}x${loop ? " · 循环" : ""}` : "实时数据"
    : mode === "paused" ? "已暂停" : mode === "finished" ? "播放完成" : Object.keys(state.topics).length ? "实时数据" : "空闲";
  workspace.updatePlayback();
}

function updateQueueStats() {
  const s = state.queueStats;
  dom.qSize.textContent = s.queue_size || 0;
  dom.qCap.textContent = s.queue_capacity || 4096;
  dom.qWatermark.textContent = Math.round((s.fill_ratio || 0) * 100) + "%";
  dom.qDropped.textContent = s.total_dropped || 0;
  dom.qBP.textContent = s.backpressure_active ? "开启" : "关闭";
  dom.qSub.textContent = (s.keep_every_n || 1) + "x";
  dom.qTotalRx.textContent = s.total_published || 0;

  // Color coding
  const zone = s.zone || "normal";
  dom.qWatermark.style.color = zone === "emergency" ? "#bf4d4d" :
    zone === "heavy" ? "#c57932" : zone === "warn" ? "#b38c34" : "#589b69";
  dom.qBP.style.color = s.backpressure_active ? "#ff9800" : "#4caf50";
}

function restartRenderLoop() {
  onFpsChanged();
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
  if (!path) { notify("请输入 .bag 文件路径。"); return; }
  const speed = parseFloat(dom.bagSpeed.value) || 1.0;
  const loop = dom.bagLoop.checked;
  sendCommand({ action: "load_bag", path, speed, loop });
};

// Sniff start
dom.startSniffBtn.onclick = () => {
  const topic = dom.liveTopic.value.trim();
  if (!topic) { notify("请输入话题名称。"); return; }
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
const updatePlaybackOptions = () => {
  if (state.playback.source === "bag") sendCommand({ action: "set_playback", speed: Number(dom.bagSpeed.value), loop: dom.bagLoop.checked });
};
dom.bagSpeed.onchange = updatePlaybackOptions;
dom.bagLoop.onchange = updatePlaybackOptions;

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
const plots = createPlotWindows({ state, onChange: syncPlotSelection, onDrop: (payload, id) => {
  if (!state.topics[payload.topic]) return;
  plots.select(id);
  if (typeof payload.field === "string" && payload.field) addField(payload.topic, payload.field, id);
  else addTopic(payload.topic, id);
} });
const workspace = createWorkspace({ state, plots, removeField, notify });
plots.init();
workspace.updatePlayback();
window.dzplotWorkspace = { state, workspace, plots, addField, removeField };
connect();
