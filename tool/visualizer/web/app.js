const state = {
  connected: false,
  socket: null,
  activeTopic: null,
  topics: new Map(),
  events: [],
  messageTypes: ["RawMessage", "Pose", "Path", "PointCloud", "Marker", "Image"],
  messageTypeMap: {
    RawMessage: "StdRawMessage",
    Pose: "StdPose",
    Path: "StdPath",
    PointCloud: "StdPointCloud",
    Marker: "StdMarker",
    Image: "StdImage",
  },
  config: {
    domain: 1,
    transport: "socket",
    queue: 10,
    poll: 0.03,
    extra: "",
    topics: [],
  },
  camera: {
    yaw: -0.75,
    pitch: 0.72,
    distance: 18,
    target: { x: 0, y: 0, z: 0 },
  },
  pointer: {
    mode: null,
    x: 0,
    y: 0,
  },
};

const maxTrail = 300;

const el = {
  connectionText: document.getElementById("connectionText"),
  topicList: document.getElementById("topicList"),
  topicCount: document.getElementById("topicCount"),
  displayMetric: document.getElementById("displayMetric"),
  activeTitle: document.getElementById("activeTitle"),
  activeMeta: document.getElementById("activeMeta"),
  zoomMetric: document.getElementById("zoomMetric"),
  eventLog: document.getElementById("eventLog"),
  sceneCanvas: document.getElementById("sceneCanvas"),
  openAddDisplayBtn: document.getElementById("openAddDisplayBtn"),
  displayModal: document.getElementById("displayModal"),
  closeDisplayModalBtn: document.getElementById("closeDisplayModalBtn"),
  cancelDisplayBtn: document.getElementById("cancelDisplayBtn"),
  addTopicBtn: document.getElementById("addTopicBtn"),
  topicInput: document.getElementById("topicInput"),
  typeInput: document.getElementById("typeInput"),
  messageTypeInput: document.getElementById("messageTypeInput"),
  domainInput: document.getElementById("domainInput"),
  queueInput: document.getElementById("queueInput"),
  transportInput: document.getElementById("transportInput"),
  pollInput: document.getElementById("pollInput"),
  extraInput: document.getElementById("extraInput"),
  exportConfigBtn: document.getElementById("exportConfigBtn"),
  importConfigBtn: document.getElementById("importConfigBtn"),
  saveConfigBtn: document.getElementById("saveConfigBtn"),
  loadConfigBtn: document.getElementById("loadConfigBtn"),
  configFileInput: document.getElementById("configFileInput"),
};

const ctx = el.sceneCanvas.getContext("2d", { alpha: false });

function wsUrl() {
  const scheme = window.location.protocol === "https:" ? "wss" : "ws";
  return `${scheme}://${window.location.host}/ws`;
}

function clamp(value, min, max) {
  return Math.max(min, Math.min(max, value));
}

function escapeHtml(value) {
  return String(value)
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;")
    .replaceAll('"', "&quot;");
}

function addEvent(kind, text) {
  state.events.unshift({ time: new Date().toLocaleTimeString(), kind, text });
  state.events = state.events.slice(0, 60);
  renderEvents();
}

function sendCommand(command) {
  if (!state.socket || state.socket.readyState !== WebSocket.OPEN) {
    addEvent("error", "websocket is not connected");
    return false;
  }
  state.socket.send(JSON.stringify(command));
  return true;
}

function ensureTopic(name, meta = {}) {
  if (!name) return null;
  if (!state.topics.has(name)) {
    state.topics.set(name, {
      name,
      type: meta.type || meta.msg_type || "",
      msg_type: meta.msg_type || meta.type || "",
      transport: meta.transport || "",
      domain: meta.domain ?? state.config.domain,
      queue: meta.queue ?? state.config.queue,
      poll: meta.poll ?? state.config.poll,
      extra: meta.extra || "",
      active: meta.active !== false,
      rate: 0,
      sampleTimes: [],
      latest: null,
      poseTrail: [],
      lastTimestamp: 0,
    });
  }
  return state.topics.get(name);
}

function setConfig(config) {
  state.config = { ...state.config, ...(config || {}) };
  el.domainInput.value = state.config.domain ?? 1;
  el.queueInput.value = state.config.queue ?? 10;
  el.transportInput.value = state.config.transport || "socket";
  el.pollInput.value = Math.round((state.config.poll ?? 0.03) * 1000);
  el.extraInput.value = state.config.extra || "";
}

function setMessageTypes(types, typeMap = null) {
  state.messageTypes = Array.isArray(types) ? types : [];
  if (typeMap && typeof typeMap === "object") {
    state.messageTypeMap = { ...state.messageTypeMap, ...typeMap };
  }
  const previous = el.typeInput.value;
  el.typeInput.innerHTML = "";
  for (const type of state.messageTypes) {
    const option = document.createElement("option");
    option.value = type;
    option.textContent = type;
    el.typeInput.appendChild(option);
  }
  if (state.messageTypes.includes(previous)) {
    el.typeInput.value = previous;
  } else if (state.messageTypes.length > 0) {
    el.typeInput.value = state.messageTypes[0];
  }
  updateResolvedMessageType();
}

function updateResolvedMessageType() {
  const displayType = el.typeInput.value;
  const resolvedType = state.messageTypeMap[displayType] || displayType || "";
  el.messageTypeInput.innerHTML = "";
  const option = document.createElement("option");
  option.value = resolvedType;
  option.textContent = resolvedType || "-";
  el.messageTypeInput.appendChild(option);
  el.messageTypeInput.value = resolvedType;
}

function currentTopicConfig() {
  return [...state.topics.values()]
    .filter((topic) => topic.active !== false)
    .map((topic) => ({
      topic: topic.name,
      msg_type: topic.msg_type || topic.type,
      domain: topic.domain,
      queue: topic.queue || state.config.queue || 10,
      transport: topic.transport || state.config.transport || "socket",
      poll: topic.poll || state.config.poll || 0.03,
      extra: topic.extra || "",
    }));
}

function buildConfigFromUi() {
  return {
    domain: Number(el.domainInput.value || state.config.domain || 1),
    transport: el.transportInput.value || state.config.transport || "socket",
    queue: Number(el.queueInput.value || state.config.queue || 10),
    poll: Number(el.pollInput.value || 30) / 1000,
    extra: el.extraInput.value || "",
    topics: currentTopicConfig(),
  };
}

function downloadJson(filename, data) {
  const blob = new Blob([JSON.stringify(data, null, 2) + "\n"], { type: "application/json" });
  const url = URL.createObjectURL(blob);
  const link = document.createElement("a");
  link.href = url;
  link.download = filename;
  document.body.appendChild(link);
  link.click();
  link.remove();
  URL.revokeObjectURL(url);
}

function getPath(obj, path) {
  const parts = path.split(".");
  let cur = obj;
  for (const part of parts) {
    if (cur == null) return undefined;
    cur = cur[part];
  }
  return cur;
}

function detectPose(data) {
  const candidates = [
    ["current_pose.x", "current_pose.y", "current_pose.z"],
    ["pose.x", "pose.y", "pose.z"],
    ["position.x", "position.y", "position.z"],
    ["x", "y", "z"],
  ];
  for (const [xPath, yPath, zPath] of candidates) {
    const x = getPath(data, xPath);
    const y = getPath(data, yPath);
    const z = getPath(data, zPath);
    if (typeof x === "number" && typeof y === "number") {
      return { x, y, z: typeof z === "number" ? z : 0 };
    }
  }
  return null;
}

function handleSample(event) {
  const topic = ensureTopic(event.topic, event);
  if (!topic) return;
  topic.type = event.msg_type || topic.type;
  topic.msg_type = event.msg_type || topic.msg_type || topic.type;
  topic.transport = event.transport || topic.transport;
  topic.domain = event.domain ?? topic.domain;
  topic.queue = event.queue ?? topic.queue;
  topic.extra = event.extra ?? topic.extra;
  topic.active = true;
  topic.latest = event.data || {};
  topic.lastTimestamp = event.timestamp_ms || Date.now();
  topic.sampleTimes.push(topic.lastTimestamp);
  if (topic.sampleTimes.length > 80) topic.sampleTimes.shift();
  if (topic.sampleTimes.length > 2) {
    const span = topic.sampleTimes[topic.sampleTimes.length - 1] - topic.sampleTimes[0];
    topic.rate = span > 0 ? ((topic.sampleTimes.length - 1) * 1000) / span : 0;
  }
  const pose = detectPose(topic.latest);
  if (pose) {
    topic.poseTrail.push(pose);
    if (topic.poseTrail.length > maxTrail) topic.poseTrail.shift();
  }
  if (!state.activeTopic) state.activeTopic = topic.name;
  renderDisplays();
  renderToolbar();
}

function connect() {
  const ws = new WebSocket(wsUrl());
  state.socket = ws;
  ws.onopen = () => {
    state.connected = true;
    el.connectionText.textContent = "Connected";
    addEvent("ws", "connected");
    renderAll();
  };
  ws.onclose = () => {
    state.connected = false;
    el.connectionText.textContent = "Disconnected, retrying";
    addEvent("ws", "disconnected");
    renderAll();
    setTimeout(connect, 1200);
  };
  ws.onerror = () => {
    state.connected = false;
    renderAll();
  };
  ws.onmessage = (message) => {
    const event = JSON.parse(message.data);
    if (event.kind === "hello") {
      setConfig(event.config || {});
      setMessageTypes(event.message_types || [], event.message_type_map || null);
      (event.topics || []).forEach((topic) => ensureTopic(topic.name || topic.topic, topic));
      renderAll();
    } else if (event.kind === "sample") {
      handleSample(event);
    } else if (event.kind === "topic_added") {
      const meta = event.topic || {};
      ensureTopic(meta.name || meta.topic, meta);
      addEvent("display", `added ${meta.name || meta.topic}`);
      renderAll();
    } else if (event.kind === "topic_removed") {
      state.topics.delete(event.topic);
      if (state.activeTopic === event.topic) state.activeTopic = null;
      addEvent("display", `removed ${event.topic}`);
      renderAll();
    } else if (event.kind === "config") {
      setConfig(event.config || {});
      setMessageTypes(event.message_types || state.messageTypes, event.message_type_map || null);
      addEvent("config", "configuration applied");
      renderAll();
    } else if (event.kind === "ack") {
      addEvent(event.ok ? "ack" : "error", event.ok ? `${event.action || "command"} ok` : event.message);
      if (event.config) setConfig(event.config);
    } else if (event.kind === "error") {
      addEvent("error", `${event.topic || "bridge"}: ${event.message}`);
    }
  };
}

function renderAll() {
  renderDisplays();
  renderToolbar();
  renderEvents();
}

function renderDisplays() {
  const topics = [...state.topics.values()]
    .filter((topic) => topic.active !== false)
    .sort((a, b) => a.name.localeCompare(b.name));
  el.topicCount.textContent = String(topics.length);
  el.displayMetric.textContent = String(topics.length);
  el.topicList.innerHTML = "";
  for (const topic of topics) {
    const button = document.createElement("button");
    button.className = `display-item ${topic.name === state.activeTopic ? "active" : ""}`;
    button.type = "button";
    button.innerHTML = `
      <div class="display-title"><span>${escapeHtml(topic.name)}</span></div>
      <div class="display-meta">${escapeHtml(topic.type || "unknown")} · ${escapeHtml(topic.transport || "-")} · ${topic.rate.toFixed(1)} Hz</div>
      <div class="display-actions"><button data-remove="${escapeHtml(topic.name)}" type="button">Remove</button></div>
    `;
    button.onclick = () => {
      state.activeTopic = topic.name;
      renderAll();
    };
    button.querySelector("[data-remove]").onclick = (event) => {
      event.stopPropagation();
      sendCommand({ action: "remove_topic", topic: topic.name });
    };
    el.topicList.appendChild(button);
  }
}

function renderToolbar() {
  const active = state.topics.get(state.activeTopic);
  el.zoomMetric.textContent = state.camera.distance.toFixed(1);
  if (!active) {
    el.activeTitle.textContent = "3D View";
    el.activeMeta.textContent = "Wheel to zoom. Right-drag to move camera. Left-drag to orbit.";
    return;
  }
  const age = active.lastTimestamp ? `${Math.max(0, Date.now() - active.lastTimestamp)} ms` : "-";
  el.activeTitle.textContent = active.name;
  el.activeMeta.textContent = `${active.type || "unknown"} · ${active.transport || "-"} · age ${age}`;
}

function renderEvents() {
  el.eventLog.innerHTML = "";
  for (const event of state.events) {
    const row = document.createElement("div");
    row.className = "event-row";
    row.innerHTML = `<span>${escapeHtml(event.time)}</span><strong>${escapeHtml(event.kind)}</strong><div></div><span>${escapeHtml(event.text)}</span>`;
    el.eventLog.appendChild(row);
  }
}

function openModal() {
  el.displayModal.hidden = false;
  updateResolvedMessageType();
  el.topicInput.focus();
}

function closeModal() {
  el.displayModal.hidden = true;
}

function addTopicFromForm() {
  const topic = el.topicInput.value.trim();
  const msgType = el.typeInput.value;
  if (!topic || !msgType) {
    addEvent("error", "topic and message type are required");
    return;
  }
  const spec = {
    topic,
    msg_type: msgType,
    domain: Number(el.domainInput.value || state.config.domain || 1),
    queue: Number(el.queueInput.value || state.config.queue || 10),
    transport: el.transportInput.value || state.config.transport || "socket",
    poll: Number(el.pollInput.value || 30) / 1000,
    extra: el.extraInput.value || "",
  };
  if (sendCommand({ action: "add_topic", topic: spec })) {
    closeModal();
  }
}

function cameraBasis() {
  const { yaw, pitch, distance, target } = state.camera;
  const cp = Math.cos(pitch);
  const eye = {
    x: target.x + distance * cp * Math.cos(yaw),
    y: target.y + distance * cp * Math.sin(yaw),
    z: target.z + distance * Math.sin(pitch),
  };
  const forward = normalize({
    x: target.x - eye.x,
    y: target.y - eye.y,
    z: target.z - eye.z,
  });
  let right = normalize(cross(forward, { x: 0, y: 0, z: 1 }));
  if (!Number.isFinite(right.x)) right = { x: 1, y: 0, z: 0 };
  const up = normalize(cross(right, forward));
  return { eye, forward, right, up };
}

function normalize(v) {
  const len = Math.hypot(v.x, v.y, v.z);
  return { x: v.x / len, y: v.y / len, z: v.z / len };
}

function cross(a, b) {
  return {
    x: a.y * b.z - a.z * b.y,
    y: a.z * b.x - a.x * b.z,
    z: a.x * b.y - a.y * b.x,
  };
}

function dot(a, b) {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}

function resizeCanvas() {
  const rect = el.sceneCanvas.getBoundingClientRect();
  const scale = window.devicePixelRatio || 1;
  const width = Math.max(1, Math.floor(rect.width * scale));
  const height = Math.max(1, Math.floor(rect.height * scale));
  if (el.sceneCanvas.width !== width || el.sceneCanvas.height !== height) {
    el.sceneCanvas.width = width;
    el.sceneCanvas.height = height;
  }
  return { width, height };
}

function project(point, basis, width, height) {
  const v = {
    x: point.x - basis.eye.x,
    y: point.y - basis.eye.y,
    z: point.z - basis.eye.z,
  };
  const z = dot(v, basis.forward);
  if (z <= 0.05) return null;
  const f = height * 0.82;
  return {
    x: width / 2 + (dot(v, basis.right) * f) / z,
    y: height / 2 - (dot(v, basis.up) * f) / z,
    z,
  };
}

function drawLine3d(a, b, basis, width, height, color, lineWidth = 1) {
  const pa = project(a, basis, width, height);
  const pb = project(b, basis, width, height);
  if (!pa || !pb) return;
  ctx.strokeStyle = color;
  ctx.lineWidth = lineWidth;
  ctx.beginPath();
  ctx.moveTo(pa.x, pa.y);
  ctx.lineTo(pb.x, pb.y);
  ctx.stroke();
}

function drawPoint3d(point, basis, width, height, color, radius = 4) {
  const p = project(point, basis, width, height);
  if (!p) return;
  ctx.fillStyle = color;
  ctx.beginPath();
  ctx.arc(p.x, p.y, radius, 0, Math.PI * 2);
  ctx.fill();
}

function renderScene() {
  const { width, height } = resizeCanvas();
  ctx.fillStyle = "#0b0f14";
  ctx.fillRect(0, 0, width, height);

  const basis = cameraBasis();
  const range = Math.max(12, Math.ceil(state.camera.distance * 1.6));
  for (let i = -range; i <= range; i += 1) {
    const color = i === 0 ? "rgba(148,163,184,0.48)" : "rgba(148,163,184,0.18)";
    drawLine3d({ x: -range, y: i, z: 0 }, { x: range, y: i, z: 0 }, basis, width, height, color, 1);
    drawLine3d({ x: i, y: -range, z: 0 }, { x: i, y: range, z: 0 }, basis, width, height, color, 1);
  }

  drawLine3d({ x: 0, y: 0, z: 0 }, { x: range, y: 0, z: 0 }, basis, width, height, "#ef4444", 3);
  drawLine3d({ x: 0, y: 0, z: 0 }, { x: 0, y: range, z: 0 }, basis, width, height, "#22c55e", 3);
  drawLine3d({ x: 0, y: 0, z: 0 }, { x: 0, y: 0, z: Math.min(8, range) }, basis, width, height, "#3b82f6", 3);
  drawPoint3d({ x: 0, y: 0, z: 0 }, basis, width, height, "#e2e8f0", 4);

  for (const topic of state.topics.values()) {
    if (topic.active === false || topic.poseTrail.length === 0) continue;
    const color = topic.name === state.activeTopic ? "#facc15" : "#38bdf8";
    for (let i = 1; i < topic.poseTrail.length; i += 1) {
      drawLine3d(topic.poseTrail[i - 1], topic.poseTrail[i], basis, width, height, color, 2);
    }
    drawPoint3d(topic.poseTrail[topic.poseTrail.length - 1], basis, width, height, color, 5);
  }

  renderToolbar();
  requestAnimationFrame(renderScene);
}

function panCamera(dx, dy) {
  const basis = cameraBasis();
  const groundForward = normalize({ x: basis.forward.x, y: basis.forward.y, z: 0 });
  const scale = state.camera.distance / Math.max(360, el.sceneCanvas.clientHeight) * 1.3;
  state.camera.target.x += (-dx * basis.right.x + dy * groundForward.x) * scale;
  state.camera.target.y += (-dx * basis.right.y + dy * groundForward.y) * scale;
  state.camera.target.z += dy * basis.up.z * scale * 0.2;
}

function setupSceneControls() {
  el.sceneCanvas.addEventListener("contextmenu", (event) => event.preventDefault());
  el.sceneCanvas.addEventListener(
    "wheel",
    (event) => {
      event.preventDefault();
      state.camera.distance = clamp(state.camera.distance * Math.exp(event.deltaY * 0.001), 3, 140);
      renderToolbar();
    },
    { passive: false },
  );
  el.sceneCanvas.addEventListener("pointerdown", (event) => {
    el.sceneCanvas.setPointerCapture(event.pointerId);
    state.pointer.x = event.clientX;
    state.pointer.y = event.clientY;
    state.pointer.mode = event.button === 2 ? "pan" : "orbit";
    el.sceneCanvas.classList.toggle("panning", state.pointer.mode === "pan");
    el.sceneCanvas.classList.toggle("orbiting", state.pointer.mode === "orbit");
  });
  el.sceneCanvas.addEventListener("pointermove", (event) => {
    if (!state.pointer.mode) return;
    const dx = event.clientX - state.pointer.x;
    const dy = event.clientY - state.pointer.y;
    state.pointer.x = event.clientX;
    state.pointer.y = event.clientY;
    if (state.pointer.mode === "pan") {
      panCamera(dx, dy);
    } else {
      state.camera.yaw -= dx * 0.006;
      state.camera.pitch = clamp(state.camera.pitch + dy * 0.006, 0.12, 1.42);
    }
  });
  el.sceneCanvas.addEventListener("pointerup", () => {
    state.pointer.mode = null;
    el.sceneCanvas.classList.remove("panning", "orbiting");
  });
  el.sceneCanvas.addEventListener("pointercancel", () => {
    state.pointer.mode = null;
    el.sceneCanvas.classList.remove("panning", "orbiting");
  });
}

el.openAddDisplayBtn.onclick = openModal;
el.closeDisplayModalBtn.onclick = closeModal;
el.cancelDisplayBtn.onclick = closeModal;
el.addTopicBtn.onclick = addTopicFromForm;
el.displayModal.addEventListener("click", (event) => {
  if (event.target === el.displayModal) closeModal();
});
for (const input of [el.topicInput, el.typeInput]) {
  input.addEventListener("keydown", (event) => {
    if (event.key === "Enter") addTopicFromForm();
  });
}
el.typeInput.onchange = updateResolvedMessageType;

el.exportConfigBtn.onclick = () => {
  const config = buildConfigFromUi();
  config.topics = currentTopicConfig();
  downloadJson("dzipc_visualizer_config.json", config);
  addEvent("config", "exported configuration");
};

el.importConfigBtn.onclick = () => el.configFileInput.click();
el.configFileInput.onchange = async () => {
  const file = el.configFileInput.files && el.configFileInput.files[0];
  if (!file) return;
  try {
    const config = JSON.parse(await file.text());
    sendCommand({ action: "apply_config", config, replace: true });
  } catch (error) {
    addEvent("error", `import failed: ${error.message}`);
  } finally {
    el.configFileInput.value = "";
  }
};
el.saveConfigBtn.onclick = () => sendCommand({ action: "save_config" });
el.loadConfigBtn.onclick = () => sendCommand({ action: "load_config" });

window.addEventListener("resize", renderToolbar);
setInterval(renderToolbar, 500);
setConfig(state.config);
setMessageTypes(state.messageTypes);
closeModal();
setupSceneControls();
connect();
requestAnimationFrame(renderScene);
