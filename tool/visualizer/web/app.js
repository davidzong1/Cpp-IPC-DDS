import * as THREE from "three";
import { OrbitControls } from "three/addons/controls/OrbitControls.js";

const state = {
  connected: false,
  socket: null,
  activeTopic: null,
  topics: new Map(),
  robotDisplays: new Map(),
  robotStateDisplays: new Map(),
  pendingBinarySamples: [],
  events: [],
  messageTypes: ["RawMessage", "Pose", "Path", "PointCloud", "Marker", "Image", "Robot", "RobotState"],
  robotStateMessageTypes: ["RobotState"],
  messageTypeMap: {
    RawMessage: "StdRawMessage",
    Pose: "StdPose",
    Path: "StdPath",
    PointCloud: "StdPointCloud",
    Marker: "StdMarker",
    Image: "StdImage",
    Robot: "",
    RobotState: "RobotState",
  },
  config: {
    domain: 1,
    transport: "socket",
    queue: 10,
    poll: 0.03,
    extra: "",
    topics: [],
    robot_displays: [],
    robot_state_displays: [],
    robot_models: [],
  },
};

const maxTrail = 300;
const displayRenderIntervalMs = 250;
const imageRenderIntervalMs = 33;
const sceneInteractionRenderMs = 900;
const maxSceneFps = 30;
const minSceneFrameMs = 1000 / maxSceneFps;
const maxPointCloudFps = 10;
const minPointCloudFrameMs = 1000 / maxPointCloudFps;
const pointCloudMapConfig = {
  voxelSize: 0.1,
  chunkSize: 10,
  maxPoints: 2000000,
  gpuUpdateIntervalMs: 500,
  maxDirtyChunksPerFrame: 8,
};
const pointCloudAdaptiveRenderConfig = {
  targetFps: 10,
  recoverFps: 13,
  maxRenderStep: 16,
  lowFpsFrames: 3,
  stableFrames: 20,
};
const sceneFpsWindowMs = 1000;
const sceneFpsUpdateIntervalMs = 250;
let displayRenderTimer = null;
let lastDisplayRender = 0;
let displayInteractionUntil = 0;
const sceneState = {
  renderer: null,
  scene: null,
  camera: null,
  controls: null,
  axisScene: null,
  axisCamera: null,
  topicObjects: new Map(),
  robotGroups: new Map(),
  grid: null,
};
const sceneRenderState = {
  // Render only when something changes; this prevents an idle WebGL view
  // from driving the GPU at the display refresh rate.
  pending: false,
  timer: null,
  scheduledAt: 0,
  lastRenderTime: 0,
  continuousUntil: 0,
  frameTimes: [],
  lastFpsUpdate: 0,
  fpsIdleTimer: null,
};
const defaultCameraPosition = new THREE.Vector3(12, -12, 10);
const topicPalette = [0x38bdf8, 0xfacc15, 0xa78bfa, 0x34d399, 0xfb7185, 0xf97316];
const sceneBackgroundColor = 0xffffff;
const gridPlaneRotation = Math.PI / 2;
const pointCloudConfig = {
  pointSize: 0.06,
  fallbackColor: 0x0f172a,
};
const binaryPointCloudHeaderBytes = 24;
const binaryPointCloudMagic = "DZPC";
const binaryPointCloudType = 1;
const binaryPointCloudHasColors = 1;
const referenceAxisConfig = {
  viewportSize: 180,
  viewportInset: 22,
  axisSize: 1.0,
  axisThickness: 50,
  labelScale: 0.5,
  cameraDistance: 4.2,
  backgroundColor: 0xffffff,
  backgroundOpacity: 0.72,
};
const axisColors = {
  x: 0xef4444,
  y: 0x22c55e,
  z: 0x3b82f6,
};

const imageState = {
  topic: null,
  width: 0,
  height: 0,
  encoding: "",
  dataB64: "",
  dataLength: 0,
  rate: 0,
  sampleTimes: [],
  renderMode: "",
  renderTimer: null,
  lastRenderTime: 0,
  manualSize: false,
  sizedTopic: null,
  sizedWidth: 0,
  sizedHeight: 0,
  dragActive: false,
  dragStartX: 0,
  dragStartY: 0,
  dragOrigLeft: 0,
  dragOrigTop: 0,
  dragNextLeft: 0,
  dragNextTop: 0,
  resizeActive: false,
  resizeStartX: 0,
  resizeStartY: 0,
  resizeOrigWidth: 0,
  resizeOrigHeight: 0,
  resizeOrigContentWidth: 0,
  resizeOrigContentHeight: 0,
  closedByUser: false,
};

const robotState = {
  nextId: 1,
};

const el = {
  connectionText: document.getElementById("connectionText"),
  topicList: document.getElementById("topicList"),
  topicCount: document.getElementById("topicCount"),
  displayMetric: document.getElementById("displayMetric"),
  sceneFpsMetric: document.getElementById("sceneFpsMetric"),
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
  urdfPathField: document.getElementById("urdfPathField"),
  urdfPathInput: document.getElementById("urdfPathInput"),
  targetRobotField: document.getElementById("targetRobotField"),
  targetRobotInput: document.getElementById("targetRobotInput"),
  exportConfigBtn: document.getElementById("exportConfigBtn"),
  importConfigBtn: document.getElementById("importConfigBtn"),
  configFileInput: document.getElementById("configFileInput"),
  imageOverlay: document.getElementById("imageOverlay"),
  imageOverlayHeader: document.getElementById("imageOverlayHeader"),
  imageOverlayTitle: document.getElementById("imageOverlayTitle"),
  imageOverlayClose: document.getElementById("imageOverlayClose"),
  imageOverlayBody: document.getElementById("imageOverlayBody"),
  imageOverlayImg: document.getElementById("imageOverlayImg"),
  imageOverlayCanvas: document.getElementById("imageOverlayCanvas"),
  imageOverlayInfo: document.getElementById("imageOverlayInfo"),
  imageResizeHandle: document.getElementById("imageResizeHandle"),
};

el.topicField = el.topicInput.closest("label");
el.messageTypeField = el.messageTypeInput.closest("label");

/* ---- 面板折叠/展开 ----
   点击 section-head 右侧的 −/+ 按钮切换面板折叠状态，
   折叠时隐藏内容（config-actions 或 event-log），切换按钮文字 */
document.querySelectorAll(".collapse-toggle").forEach((btn) => {
  btn.addEventListener("click", () => {
    const panel = btn.closest("section");
    const collapsed = panel.classList.toggle("collapsed");
    btn.textContent = collapsed ? "+" : "−";             // 折叠时显示 +，展开时显示 −
    btn.setAttribute("aria-label", collapsed ? "Expand" : "Collapse");
  });
});

/* ---- 侧边栏宽度拖拽 resize ----
   拖拽右侧灰色手柄调整整个侧边栏宽度，范围 200~600px，
   通过修改 .app-shell 的 grid-template-columns 实现 */
(() => {
  const sidebar = document.querySelector(".sidebar");
  const handle = document.querySelector(".sidebar-resize-handle");
  const shell = document.querySelector(".app-shell");
  if (!handle || !sidebar || !shell) return;

  let dragging = false;
  let startX = 0;
  let startWidth = 0;
  const minWidth = 200;   // 最小侧边栏宽度
  const maxWidth = 600;   // 最大侧边栏宽度

  handle.addEventListener("mousedown", (e) => {
    if (e.button !== 0) return;  // 仅左键
    e.preventDefault();
    dragging = true;
    handle.classList.add("dragging");
    startX = e.clientX;
    startWidth = sidebar.getBoundingClientRect().width;
    document.body.style.cursor = "col-resize";
    document.body.style.userSelect = "none";  // 拖拽时禁止选中文字
  });

  window.addEventListener("mousemove", (e) => {
    if (!dragging) return;
    const delta = e.clientX - startX;
    const newWidth = Math.max(minWidth, Math.min(maxWidth, startWidth + delta));
    shell.style.gridTemplateColumns = `${newWidth}px minmax(0, 1fr)`;
  });

  window.addEventListener("mouseup", () => {
    if (!dragging) return;
    handle.classList.remove("dragging");
    dragging = false;
    document.body.style.cursor = "";
    document.body.style.userSelect = "";
  });
})();

function wsUrl() {
  const scheme = window.location.protocol === "https:" ? "wss" : "ws";
  return `${scheme}://${window.location.host}/ws`;
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
      pointCloud: null,
      visualize: meta.visualize === true,
      displayType: meta.display_type || meta.displayType || "",
      robotId: meta.robot_id || meta.robotId || "",
      displayId: meta.display_id || meta.displayId || "",
      lastTimestamp: 0,
    });
  } else {
    const topic = state.topics.get(name);
    topic.displayType = meta.display_type || meta.displayType || topic.displayType || "";
    topic.robotId = meta.robot_id || meta.robotId || topic.robotId || "";
    topic.displayId = meta.display_id || meta.displayId || topic.displayId || "";
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
  setRobotDisplaysFromConfig(state.config);
  updateDisplayForm();
}

function makeRobotPlaceholder(raw) {
  return {
    id: raw.id || raw.name || `robot_${robotState.nextId++}`,
    name: raw.name || raw.id || "Robot",
    fileName: raw.file_name || raw.urdf_path || "",
    urdfPath: raw.urdf_path || raw.file_name || "",
    urdf: raw.urdf || "",
    visible: raw.visible !== false,
    links: [],
    joints: [],
    roots: [],
    linkPoses: new Map(),
    jointPoses: new Map(),
    warnings: raw.urdf ? [] : ["URDF is not loaded in browser yet"],
  };
}

function robotDisplayFromConfig(raw) {
  if (raw?.urdf) {
    const model = parseUrdfRobot(raw.urdf, raw.urdf_path || raw.file_name || raw.name || "");
    model.id = raw.id || model.id;
    model.name = raw.name || model.name;
    model.fileName = raw.file_name || raw.urdf_path || model.fileName || "";
    model.urdfPath = raw.urdf_path || raw.file_name || "";
    model.visible = raw.visible !== false;
    model.displayType = "Robot";
    return model;
  }
  return { ...makeRobotPlaceholder(raw || {}), displayType: "Robot" };
}

function setRobotDisplaysFromConfig(config) {
  state.robotDisplays.clear();
  state.robotStateDisplays.clear();
  const robotItems = Array.isArray(config.robot_displays)
    ? config.robot_displays
    : (Array.isArray(config.robot_models) ? config.robot_models : []);
  for (const item of robotItems) {
    const model = robotDisplayFromConfig(item);
    state.robotDisplays.set(model.id, model);
  }
  for (const item of config.robot_state_displays || []) {
    state.robotStateDisplays.set(item.id || item.topic, { ...item });
  }
  const visibleRobot = [...state.robotDisplays.values()].find((robot) => robot.visible !== false && robot.urdf);
  state.activeTopic = visibleRobot ? `robot:${visibleRobot.id}` : state.activeTopic;
  syncRobotObject();
  requestSceneRender();
}

function setMessageTypes(types, typeMap = null) {
  const baseTypes = Array.isArray(types) ? types : [];
  state.messageTypes = [...baseTypes.filter((type) => type !== "Robot" && type !== "RobotState"), "Robot", "RobotState"];
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
  const isRobot = displayType === "Robot";
  const isRobotState = displayType === "RobotState";
  const resolvedType = isRobot
    ? ""
    : isRobotState
      ? state.robotStateMessageTypes[0]
      : state.messageTypeMap[displayType] || displayType || "";
  el.messageTypeInput.innerHTML = "";
  el.urdfPathField.hidden = !isRobot;
  el.targetRobotField.hidden = !isRobotState;
  el.topicField.hidden = false;
  el.messageTypeField.hidden = isRobot;
  el.topicInput.placeholder = isRobot ? "robot" : "robot_state";
  el.messageTypeInput.disabled = true;  /* 消息类型由类型自动推导，不可手动选择 */
  if (isRobot) {
    el.messageTypeInput.value = "";
  } else if (isRobotState) {
    for (const type of state.robotStateMessageTypes) {
      const option = document.createElement("option");
      option.value = type;
      option.textContent = type;
      el.messageTypeInput.appendChild(option);
    }
    el.messageTypeInput.value = state.robotStateMessageTypes.includes(resolvedType)
      ? resolvedType
      : state.robotStateMessageTypes[0];
  } else {
    const option = document.createElement("option");
    option.value = resolvedType;
    option.textContent = resolvedType || "-";
    el.messageTypeInput.appendChild(option);
    el.messageTypeInput.value = resolvedType;
  }
  for (const field of [el.domainInput, el.queueInput, el.transportInput, el.pollInput, el.extraInput]) {
    field.closest("label").hidden = isRobot;
  }
  updateTargetRobotOptions();
}

function updateDisplayForm() {
  updateResolvedMessageType();
}

function updateTargetRobotOptions() {
  const previous = el.targetRobotInput.value;
  el.targetRobotInput.innerHTML = "";
  const robotEntries = [...state.robotDisplays.values()].sort((a, b) => a.name.localeCompare(b.name));
  for (const robot of robotEntries) {
    const option = document.createElement("option");
    option.value = robot.id;
    option.textContent = robot.name;
    el.targetRobotInput.appendChild(option);
  }
  if (state.robotDisplays.has(previous)) {
    el.targetRobotInput.value = previous;
  }
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

function currentRobotModelsConfig() {
  return currentRobotDisplayConfig().map((robot) => ({
    id: robot.id,
    name: robot.name,
    file_name: robot.urdf_path || robot.file_name || "",
    urdf_path: robot.urdf_path || "",
    urdf: robot.urdf,
    visible: robot.visible,
  }));
}

function currentRobotDisplayConfig() {
  return [...state.robotDisplays.values()].map((robot) => ({
    id: robot.id,
    display_type: "Robot",
    name: robot.name,
    topic: robot.name,
    urdf_path: robot.urdfPath || robot.fileName || "",
    urdf: robot.urdf || "",
    visible: robot.visible !== false,
    fixed_frame: robot.fixedFrame || "world",
  }));
}

function currentRobotStateDisplayConfig() {
  return [...state.robotStateDisplays.values()].map((display) => ({ ...display }));
}

function buildConfigFromUi() {
  return {
    domain: Number(el.domainInput.value || state.config.domain || 1),
    transport: el.transportInput.value || state.config.transport || "socket",
    queue: Number(el.queueInput.value || state.config.queue || 10),
    poll: Number(el.pollInput.value || 30) / 1000,
    extra: el.extraInput.value || "",
    topics: currentTopicConfig(),
    robot_displays: currentRobotDisplayConfig(),
    robot_state_displays: currentRobotStateDisplayConfig(),
    robot_models: currentRobotModelsConfig(),
  };
}

function syncDisplayConfig() {
  state.config.robot_displays = currentRobotDisplayConfig();
  state.config.robot_state_displays = currentRobotStateDisplayConfig();
  state.config.robot_models = currentRobotModelsConfig();
}

function upsertRobotDisplay(raw, options = {}) {
  const model = robotDisplayFromConfig(raw);
  state.robotDisplays.set(model.id, model);
  if (options.persist !== false) syncDisplayConfig();
  syncRobotObject();
  requestSceneRender();
  renderDisplays();
  renderToolbar();
  updateDisplayForm();
  return model;
}

function removeRobotDisplay(robotId, options = {}) {
  const model = state.robotDisplays.get(robotId);
  if (!model) return;
  state.robotDisplays.delete(robotId);
  for (const [id, display] of state.robotStateDisplays.entries()) {
    if (display.target_robot_id === robotId || display.targetRobotId === robotId) {
      state.robotStateDisplays.delete(id);
    }
  }
  if (options.persist !== false) syncDisplayConfig();
  syncRobotObject();
  requestSceneRender();
  renderDisplays();
  renderToolbar();
  updateDisplayForm();
}

function upsertRobotStateDisplay(raw, options = {}) {
  const targetRobotId = raw.target_robot_id || raw.targetRobotId || "";
  if (!targetRobotId || !state.robotDisplays.has(targetRobotId)) {
    throw new Error("robot_state requires an existing robot display");
  }
  const displayId = raw.id || raw.topic || `robot_state_${robotState.nextId++}`;
  const display = {
    id: displayId,
    display_type: "RobotState",
    name: raw.name || raw.topic || displayId,
    topic: raw.topic || raw.name || displayId,
    msg_type: raw.msg_type || "RobotState",
    transport: raw.transport || state.config.transport || "socket",
    domain: raw.domain ?? state.config.domain ?? 1,
    queue: raw.queue ?? state.config.queue ?? 10,
    poll: raw.poll ?? state.config.poll ?? 0.03,
    extra: raw.extra || "",
    target_robot_id: targetRobotId,
    targetRobotId,
    active: raw.active !== false,
  };
  state.robotStateDisplays.set(display.id, display);
  if (options.persist !== false) syncDisplayConfig();
  requestSceneRender();
  renderDisplays();
  renderToolbar();
  updateDisplayForm();
  return display;
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
  const robotStateDisplay = [...state.robotStateDisplays.values()].find((display) => display.topic === topic.name);
  topic.type = event.msg_type || topic.type;
  topic.msg_type = event.msg_type || topic.msg_type || topic.type;
  topic.transport = event.transport || topic.transport;
  topic.domain = event.domain ?? topic.domain;
  topic.queue = event.queue ?? topic.queue;
  topic.extra = event.extra ?? topic.extra;
  topic.active = true;
  topic.latest = event.data || {};
  topic.lastTimestamp = event.timestamp_ms || Date.now();
  if (robotStateDisplay) {
    robotStateDisplay.latest = topic.latest;
    robotStateDisplay.lastTimestamp = topic.lastTimestamp;
    robotStateDisplay.rate = topic.rate;
  }
  topic.sampleTimes.push(topic.lastTimestamp);
  if (topic.sampleTimes.length > 80) topic.sampleTimes.shift();
  if (topic.sampleTimes.length > 2) {
    const span = topic.sampleTimes[topic.sampleTimes.length - 1] - topic.sampleTimes[0];
    topic.rate = span > 0 ? ((topic.sampleTimes.length - 1) * 1000) / span : 0;
  }
  if (robotStateDisplay) robotStateDisplay.rate = topic.rate;
  const pose = detectPose(topic.latest);
  if (pose) {
    topic.poseTrail.push(pose);
    if (topic.poseTrail.length > maxTrail) topic.poseTrail.shift();
  }
  if (event.binary?.encoding === "dzpc.pointcloud.v1") {
    state.pendingBinarySamples.push({
      sampleId: event.binary.sample_id,
      topicName: topic.name,
      metadata: event.binary,
    });
    if (state.pendingBinarySamples.length > 32) state.pendingBinarySamples.shift();
  }
  if (event.binary?.encoding_type === "dzpc.image.v1") {
    topic.imageData = {
      width: event.binary.width,
      height: event.binary.height,
      encoding: event.binary.image_encoding,
      dataB64: event.binary.data_b64,
      dataLength: event.binary.data_length,
    };
    topic.imageTimes = topic.imageTimes || [];
    topic.imageTimes.push(topic.lastTimestamp);
    if (topic.imageTimes.length > 40) topic.imageTimes.shift();
  }
  if (!state.activeTopic) state.activeTopic = topic.name;
  requestDisplayRender();
  if (topic.visualize && pose) requestSceneRender();
  renderToolbar();
  updateImageOverlay();
}

function pointCloudMapColor(cloud, index) {
  if (cloud.colors) {
    return [
      cloud.colors[index * 3],
      cloud.colors[index * 3 + 1],
      cloud.colors[index * 3 + 2],
    ];
  }
  return [
    ((pointCloudConfig.fallbackColor >> 16) & 0xff) / 255,
    ((pointCloudConfig.fallbackColor >> 8) & 0xff) / 255,
    (pointCloudConfig.fallbackColor & 0xff) / 255,
  ];
}

function makeGridKey(x, y, z, size) {
  return `${Math.floor(x / size)},${Math.floor(y / size)},${Math.floor(z / size)}`;
}

function ensurePointCloudMap(topic) {
  if (!topic.pointCloudMap) {
    topic.pointCloudMap = {
      chunks: new Map(),
      dirtyChunks: new Set(),
      voxels: new Set(),
      totalPoints: 0,
      lastGpuUpdate: 0,
      capped: false,
    };
  }
  return topic.pointCloudMap;
}

function ensurePointCloudRenderBudget(topic) {
  if (!topic.pointCloudRenderBudget) {
    topic.pointCloudRenderBudget = {
      latestStep: 1,
      mapStep: 1,
      lowFpsFrames: 0,
      stableFrames: 0,
    };
  }
  return topic.pointCloudRenderBudget;
}

function currentSceneFps(renderTime = performance.now()) {
  const cutoff = renderTime - sceneFpsWindowMs;
  let frames = 0;
  for (let index = sceneRenderState.frameTimes.length - 1; index >= 0; index -= 1) {
    if (sceneRenderState.frameTimes[index] < cutoff) break;
    frames += 1;
  }
  return frames * (1000 / sceneFpsWindowMs);
}

function markPointCloudMapDirty(topic) {
  const map = topic.pointCloudMap;
  if (!map) return;
  for (const [chunkKey, chunk] of map.chunks.entries()) {
    chunk.dirty = true;
    map.dirtyChunks.add(chunkKey);
  }
  requestPointCloudMapRender();
}

function tunePointCloudRenderBudget(topic) {
  const budget = ensurePointCloudRenderBudget(topic);
  // The full point arrays stay in memory. Only the GPU upload stride changes
  // when rendering cannot hold the target point-cloud frame rate.
  const fps = currentSceneFps();
  if (sceneRenderState.frameTimes.length < 3 || fps <= 0) return budget;

  const oldLatestStep = budget.latestStep;
  const oldMapStep = budget.mapStep;
  if (fps < pointCloudAdaptiveRenderConfig.targetFps) {
    budget.lowFpsFrames += 1;
    budget.stableFrames = 0;
    if (budget.lowFpsFrames >= pointCloudAdaptiveRenderConfig.lowFpsFrames) {
      budget.latestStep = Math.min(
        pointCloudAdaptiveRenderConfig.maxRenderStep,
        budget.latestStep * 2,
      );
      budget.mapStep = Math.min(
        pointCloudAdaptiveRenderConfig.maxRenderStep,
        budget.mapStep * 2,
      );
      budget.lowFpsFrames = 0;
    }
  } else if (fps >= pointCloudAdaptiveRenderConfig.recoverFps) {
    budget.stableFrames += 1;
    budget.lowFpsFrames = 0;
    if (budget.stableFrames >= pointCloudAdaptiveRenderConfig.stableFrames) {
      budget.latestStep = Math.max(1, Math.floor(budget.latestStep / 2));
      budget.mapStep = Math.max(1, Math.floor(budget.mapStep / 2));
      budget.stableFrames = 0;
    }
  } else {
    budget.lowFpsFrames = 0;
    budget.stableFrames = 0;
  }

  if (budget.mapStep !== oldMapStep) markPointCloudMapDirty(topic);
  if (budget.latestStep !== oldLatestStep) requestPointCloudRender();
  return budget;
}

function sampledFloat32Array(values, itemSize, step) {
  if (step <= 1) return values instanceof Float32Array ? values : new Float32Array(values);
  const itemCount = Math.floor(values.length / itemSize);
  const sampledCount = Math.ceil(itemCount / step);
  const sampled = new Float32Array(sampledCount * itemSize);
  let dst = 0;
  for (let srcItem = 0; srcItem < itemCount; srcItem += step) {
    const src = srcItem * itemSize;
    for (let offset = 0; offset < itemSize; offset += 1) {
      sampled[dst] = values[src + offset];
      dst += 1;
    }
  }
  return sampled;
}

function accumulatePointCloudMap(topic, cloud) {
  const map = ensurePointCloudMap(topic);
  const voxelSize = pointCloudMapConfig.voxelSize;
  const chunkSize = pointCloudMapConfig.chunkSize;
  const positions = cloud.positions;
  for (let index = 0; index < cloud.pointCount; index += 1) {
    if (map.totalPoints >= pointCloudMapConfig.maxPoints) {
      map.capped = true;
      break;
    }
    const offset = index * 3;
    const x = positions[offset];
    const y = positions[offset + 1];
    const z = positions[offset + 2];
    if (!Number.isFinite(x) || !Number.isFinite(y) || !Number.isFinite(z)) continue;
    const voxelKey = makeGridKey(x, y, z, voxelSize);
    if (map.voxels.has(voxelKey)) continue;
    map.voxels.add(voxelKey);

    const chunkKey = makeGridKey(x, y, z, chunkSize);
    let chunk = map.chunks.get(chunkKey);
    if (!chunk) {
      chunk = { key: chunkKey, positions: [], colors: [], points: null, dirty: true };
      map.chunks.set(chunkKey, chunk);
    }
    chunk.positions.push(x, y, z);
    chunk.colors.push(...pointCloudMapColor(cloud, index));
    chunk.dirty = true;
    map.dirtyChunks.add(chunkKey);
    map.totalPoints += 1;
  }
  if (map.dirtyChunks.size > 0) requestPointCloudMapRender();
}

function handleBinaryFrame(buffer) {
  if (!(buffer instanceof ArrayBuffer) || buffer.byteLength < binaryPointCloudHeaderBytes) {
    addEvent("error", "invalid binary frame");
    return;
  }
  const view = new DataView(buffer);
  const magic = String.fromCharCode(view.getUint8(0), view.getUint8(1), view.getUint8(2), view.getUint8(3));
  const version = view.getUint8(4);
  const type = view.getUint8(5);
  const flags = view.getUint16(6, true);
  const sampleId = view.getUint32(8, true);
  const pointCount = view.getUint32(12, true);
  const colorCount = view.getUint32(16, true);
  if (magic !== binaryPointCloudMagic || version !== 1 || type !== binaryPointCloudType) {
    addEvent("error", "unsupported binary frame");
    return;
  }
  const hasColors = Boolean(flags & binaryPointCloudHasColors) && colorCount === pointCount;
  const pointBytes = pointCount * 3 * Float32Array.BYTES_PER_ELEMENT;
  const colorOffset = binaryPointCloudHeaderBytes + pointBytes;
  const colorBytes = hasColors ? pointCount * 4 * Float32Array.BYTES_PER_ELEMENT : 0;
  if (buffer.byteLength < colorOffset + colorBytes) {
    addEvent("error", "truncated point cloud frame");
    return;
  }
  const pendingIndex = state.pendingBinarySamples.findIndex((item) => item.sampleId === sampleId);
  if (pendingIndex < 0) {
    addEvent("error", `unmatched point cloud frame ${sampleId}`);
    return;
  }
  const pending = state.pendingBinarySamples.splice(pendingIndex, 1)[0];
  const topic = state.topics.get(pending.topicName);
  if (!topic) return;

  const positions = new Float32Array(buffer, binaryPointCloudHeaderBytes, pointCount * 3);
  let colors = null;
  if (hasColors) {
    const rawColors = new Float32Array(buffer, colorOffset, pointCount * 4);
    colors = new Float32Array(pointCount * 3);
    for (let src = 0, dst = 0; src < rawColors.length; src += 4, dst += 3) {
      colors[dst] = rawColors[src];
      colors[dst + 1] = rawColors[src + 1];
      colors[dst + 2] = rawColors[src + 2];
    }
  }
  topic.pointCloud = {
    sampleId,
    pointCount,
    positions,
    colors,
    hasColors,
    metadata: pending.metadata,
  };
  topic.lastTimestamp = Date.now();
  accumulatePointCloudMap(topic, topic.pointCloud);
  topic.latest = {
    ...(topic.latest || {}),
    point_count: pointCount,
    map_points: topic.pointCloudMap?.totalPoints || 0,
    binary_points: true,
  };
  requestPointCloudRender();
  renderDisplays();
  renderToolbar();
}

function connect() {
  const ws = new WebSocket(wsUrl());
  ws.binaryType = "arraybuffer";
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
    if (message.data instanceof ArrayBuffer) {
      handleBinaryFrame(message.data);
      return;
    }
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
    } else if (event.kind === "robot_added" || event.kind === "robot_updated") {
      const model = upsertRobotDisplay(event.robot || {}, { persist: false });
      addEvent("display", `${event.kind === "robot_added" ? "added" : "updated"} ${model.name}`);
      renderAll();
    } else if (event.kind === "robot_removed") {
      removeRobotDisplay(event.robot_id, { persist: false });
      addEvent("display", `removed ${event.robot_id}`);
      renderAll();
    } else if (event.kind === "robot_state_added") {
      const item = event.robot_state || {};
      upsertRobotStateDisplay(item, { persist: false });
      addEvent("display", `added ${item.name || item.topic}`);
      renderAll();
    } else if (event.kind === "robot_state_removed") {
      const removedDisplay = state.robotStateDisplays.get(event.robot_state_id);
      state.robotStateDisplays.delete(event.robot_state_id);
      if (removedDisplay?.topic) state.topics.delete(removedDisplay.topic);
      syncRobotObject();
      addEvent("display", `removed ${event.robot_state_id}`);
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
  updateDisplayForm();
  requestSceneRender();
  updateImageOverlay();
}

function requestDisplayRender() {
  const interactionDelay = displayInteractionUntil - Date.now();
  if (interactionDelay > 0) {
    if (!displayRenderTimer) {
      displayRenderTimer = window.setTimeout(() => {
        displayRenderTimer = null;
        requestDisplayRender();
      }, interactionDelay);
    }
    return;
  }
  const elapsed = Date.now() - lastDisplayRender;
  if (elapsed >= displayRenderIntervalMs) {
    renderDisplays();
    return;
  }
  if (displayRenderTimer) return;
  displayRenderTimer = window.setTimeout(() => {
    displayRenderTimer = null;
    renderDisplays();
  }, displayRenderIntervalMs - elapsed);
}

function holdDisplayInteraction(ms = 700) {
  displayInteractionUntil = Math.max(displayInteractionUntil, Date.now() + ms);
}

function resetImageOverlayAutoSize(topicName) {
  if (imageState.topic && imageState.topic !== topicName) return;
  imageState.manualSize = false;
  imageState.sizedTopic = null;
  imageState.sizedWidth = 0;
  imageState.sizedHeight = 0;
}

function setTopicVisualization(topic, visualize) {
  holdDisplayInteraction();
  topic.visualize = visualize;
  state.activeTopic = topic.name;
  if (topic.visualize) {
    resetImageOverlayAutoSize(topic.name);
    imageState.topic = null;
    imageState.dataB64 = "";
  } else if (imageState.topic === topic.name) {
    hideImageOverlay();
  }
  renderDisplays();
  renderToolbar();
  updateImageOverlay();
}

function renderDisplays() {
  if (displayRenderTimer) {
    window.clearTimeout(displayRenderTimer);
    displayRenderTimer = null;
  }
  lastDisplayRender = Date.now();
  const robotStateTopics = new Set([...state.robotStateDisplays.values()].map((display) => display.topic));
  const topics = [...state.topics.values()]
    .filter((topic) => topic.active !== false && !robotStateTopics.has(topic.name))
    .sort((a, b) => a.name.localeCompare(b.name));
  const robots = [...state.robotDisplays.values()].sort((a, b) => a.name.localeCompare(b.name));
  const robotStates = [...state.robotStateDisplays.values()].sort((a, b) => a.name.localeCompare(b.name));
  const displayCount = topics.length + robots.length + robotStates.length;
  el.topicCount.textContent = String(displayCount);
  el.displayMetric.textContent = String(displayCount);
  el.topicList.innerHTML = "";
  for (const robot of robots) {
    const row = document.createElement("div");
    row.className = `display-item ${state.activeTopic === `robot:${robot.id}` ? "active" : ""}`;
    row.role = "button";
    row.tabIndex = 0;
    row.innerHTML = `
      <div class="display-title"><span>${escapeHtml(robot.name)}</span></div>
      <div class="display-meta">Robot · ${escapeHtml(robot.urdfPath || robot.fileName || "URDF")} · ${robot.links?.length || 0} links</div>
      <div class="display-actions">
        <label class="visualize-toggle">
          <input data-robot-visualize="${escapeHtml(robot.id)}" type="checkbox" ${robot.visible !== false ? "checked" : ""} />
          <span>Visualize</span>
        </label>
        <button data-remove-robot="${escapeHtml(robot.id)}" type="button">Remove</button>
      </div>
    `;
    row.onclick = () => {
      state.activeTopic = `robot:${robot.id}`;
      syncRobotObject();
      renderAll();
    };
    const toggle = row.querySelector(".visualize-toggle");
    const input = row.querySelector("[data-robot-visualize]");
    const currentRobotConfig = () => currentRobotDisplayConfig().find((item) => item.id === robot.id);
    toggle.onpointerdown = (event) => {
      event.preventDefault();
      event.stopPropagation();
      robot.visible = !(robot.visible !== false);
      syncDisplayConfig();
      syncRobotObject();
      requestSceneRender();
      sendCommand({ action: "add_robot", robot: currentRobotConfig() });
      renderDisplays();
    };
    input.onchange = (event) => {
      event.stopPropagation();
      robot.visible = event.target.checked;
      syncDisplayConfig();
      syncRobotObject();
      requestSceneRender();
      sendCommand({ action: "add_robot", robot: currentRobotConfig() });
      renderDisplays();
    };
    toggle.onclick = (event) => {
      event.preventDefault();
      event.stopPropagation();
    };
    row.querySelector("[data-remove-robot]").onclick = (event) => {
      event.stopPropagation();
      sendCommand({ action: "remove_robot", robot_id: robot.id });
    };
    el.topicList.appendChild(row);
  }
  for (const robotStateDisplay of robotStates) {
    const topic = state.topics.get(robotStateDisplay.topic);
    const rate = topic?.rate ?? robotStateDisplay.rate ?? 0;
    const row = document.createElement("div");
    row.className = `display-item ${robotStateDisplay.topic === state.activeTopic ? "active" : ""}`;
    row.role = "button";
    row.tabIndex = 0;
    row.innerHTML = `
      <div class="display-title"><span>${escapeHtml(robotStateDisplay.name)}</span></div>
      <div class="display-meta">RobotState · ${escapeHtml(robotStateDisplay.target_robot_id || robotStateDisplay.targetRobotId || "-")} · ${escapeHtml(robotStateDisplay.msg_type || "RobotState")} · ${rate.toFixed(1)} Hz</div>
      <div class="display-actions">
        <label class="visualize-toggle">
          <input data-state-visualize="${escapeHtml(robotStateDisplay.id)}" type="checkbox" ${robotStateDisplay.active !== false ? "checked" : ""} />
          <span>Visualize</span>
        </label>
        <button data-remove-state="${escapeHtml(robotStateDisplay.id)}" type="button">Remove</button>
      </div>
    `;
    row.onclick = () => {
      state.activeTopic = robotStateDisplay.topic;
      renderAll();
    };
    row.onkeydown = (event) => {
      if (event.target !== row) return;
      if (event.key !== "Enter" && event.key !== " ") return;
      event.preventDefault();
      state.activeTopic = robotStateDisplay.topic;
      renderAll();
    };
    const visualizeInput = row.querySelector("[data-state-visualize]");
    const visualizeToggle = row.querySelector(".visualize-toggle");
    visualizeToggle.onpointerdown = (event) => {
      event.preventDefault();
      event.stopPropagation();
      robotStateDisplay.active = !robotStateDisplay.active;
      syncDisplayConfig();
      requestSceneRender();
      renderDisplays();
    };
    visualizeInput.onchange = (event) => {
      event.stopPropagation();
      robotStateDisplay.active = event.target.checked;
      syncDisplayConfig();
      requestSceneRender();
      renderDisplays();
    };
    row.querySelector("[data-remove-state]").onclick = (event) => {
      event.stopPropagation();
      sendCommand({ action: "remove_robot_state", robot_state_id: robotStateDisplay.id });
      sendCommand({ action: "remove_topic", topic: robotStateDisplay.topic });
    };
    el.topicList.appendChild(row);
  }
  for (const topic of topics) {
    const row = document.createElement("div");
    row.className = `display-item ${topic.name === state.activeTopic ? "active" : ""}`;
    row.role = "button";
    row.tabIndex = 0;
    row.innerHTML = `
      <div class="display-title"><span>${escapeHtml(topic.name)}</span></div>
      <div class="display-meta">${escapeHtml(topic.displayType || topic.display_type || topic.type || "unknown")} · ${escapeHtml(topic.robotId ? `robot ${topic.robotId}` : topic.transport || "-")} · ${topic.rate.toFixed(1)} Hz</div>
      <div class="display-actions">
        <label class="visualize-toggle">
          <input data-visualize="${escapeHtml(topic.name)}" type="checkbox" ${topic.visualize ? "checked" : ""} />
          <span>Visualize</span>
        </label>
        <button data-remove="${escapeHtml(topic.name)}" type="button">Remove</button>
      </div>
    `;
    row.onclick = () => {
      state.activeTopic = topic.name;
      renderAll();
    };
    row.onkeydown = (event) => {
      if (event.target !== row) return;
      if (event.key !== "Enter" && event.key !== " ") return;
      event.preventDefault();
      state.activeTopic = topic.name;
      renderAll();
    };
    const visualizeInput = row.querySelector("[data-visualize]");
    const visualizeToggle = row.querySelector(".visualize-toggle");
    visualizeToggle.onpointerdown = (event) => {
      event.preventDefault();
      event.stopPropagation();
      setTopicVisualization(topic, !topic.visualize);
    };
    visualizeToggle.onkeydown = (event) => {
      if (event.key !== "Enter" && event.key !== " ") return;
      event.preventDefault();
      event.stopPropagation();
      setTopicVisualization(topic, !topic.visualize);
    };
    visualizeInput.onchange = (event) => {
      event.stopPropagation();
      setTopicVisualization(topic, event.target.checked);
    };
    visualizeToggle.onclick = (event) => {
      event.preventDefault();
      event.stopPropagation();
    };
    row.querySelector("[data-remove]").onclick = (event) => {
      event.stopPropagation();
      sendCommand({ action: "remove_topic", topic: topic.name });
    };
    el.topicList.appendChild(row);
  }
}

function renderToolbar() {
  const active = state.topics.get(state.activeTopic);
  const distance =
    sceneState.camera && sceneState.controls
      ? sceneState.camera.position.distanceTo(sceneState.controls.target)
      : defaultCameraPosition.length();
  el.zoomMetric.textContent = distance.toFixed(1);
  if (!active) {
    const visibleRobot = [...state.robotDisplays.values()].find((robot) => robot.visible !== false && robot.urdf);
    el.activeTitle.textContent = visibleRobot ? visibleRobot.name : "3D View";
    el.activeMeta.textContent = visibleRobot
      ? `URDF robot · ${visibleRobot.links?.length || 0} links · ${visibleRobot.joints?.length || 0} joints`
      : "Wheel to zoom. Right-drag to move camera. Left-drag to orbit.";
    return;
  }
  const age = active.lastTimestamp ? `${Math.max(0, Date.now() - active.lastTimestamp)} ms` : "-";
  el.activeTitle.textContent = active.name;
  el.activeMeta.textContent = `${active.displayType || active.type || "unknown"} · ${active.robotId ? `robot ${active.robotId}` : active.transport || "-"} · age ${age}`;
}

function isImageType(type) {
  return type === "Image" || type === "StdImage";
}

function decodeBase64(b64) {
  const binary = atob(b64);
  const bytes = new Uint8Array(binary.length);
  for (let i = 0; i < binary.length; i++) {
    bytes[i] = binary.charCodeAt(i);
  }
  return bytes;
}

function renderRawImageToCanvas(bytes, width, height, encoding, step, canvas) {
  const ctx = canvas.getContext("2d");
  canvas.width = width;
  canvas.height = height;
  const imageData = ctx.createImageData(width, height);
  const dst = imageData.data;
  const rowStride = step > 0 ? step : width * 3;

  if (encoding === "rgb8") {
    for (let y = 0; y < height; y++) {
      for (let x = 0; x < width; x++) {
        const srcOff = y * rowStride + x * 3;
        const dstOff = (y * width + x) * 4;
        dst[dstOff] = bytes[srcOff];
        dst[dstOff + 1] = bytes[srcOff + 1];
        dst[dstOff + 2] = bytes[srcOff + 2];
        dst[dstOff + 3] = 255;
      }
    }
  } else if (encoding === "bgr8") {
    for (let y = 0; y < height; y++) {
      for (let x = 0; x < width; x++) {
        const srcOff = y * rowStride + x * 3;
        const dstOff = (y * width + x) * 4;
        dst[dstOff] = bytes[srcOff + 2];
        dst[dstOff + 1] = bytes[srcOff + 1];
        dst[dstOff + 2] = bytes[srcOff];
        dst[dstOff + 3] = 255;
      }
    }
  } else if (encoding === "rgba8") {
    const srcStep = step > 0 ? step : width * 4;
    for (let y = 0; y < height; y++) {
      for (let x = 0; x < width; x++) {
        const srcOff = y * srcStep + x * 4;
        const dstOff = (y * width + x) * 4;
        dst[dstOff] = bytes[srcOff];
        dst[dstOff + 1] = bytes[srcOff + 1];
        dst[dstOff + 2] = bytes[srcOff + 2];
        dst[dstOff + 3] = bytes[srcOff + 3];
      }
    }
  } else if (encoding === "bgra8") {
    const srcStep = step > 0 ? step : width * 4;
    for (let y = 0; y < height; y++) {
      for (let x = 0; x < width; x++) {
        const srcOff = y * srcStep + x * 4;
        const dstOff = (y * width + x) * 4;
        dst[dstOff] = bytes[srcOff + 2];
        dst[dstOff + 1] = bytes[srcOff + 1];
        dst[dstOff + 2] = bytes[srcOff];
        dst[dstOff + 3] = bytes[srcOff + 3];
      }
    }
  } else if (encoding === "mono8") {
    const srcStep = step > 0 ? step : width;
    for (let y = 0; y < height; y++) {
      for (let x = 0; x < width; x++) {
        const srcOff = y * srcStep + x;
        const dstOff = (y * width + x) * 4;
        const v = bytes[srcOff];
        dst[dstOff] = v;
        dst[dstOff + 1] = v;
        dst[dstOff + 2] = v;
        dst[dstOff + 3] = 255;
      }
    }
  } else {
    return false;
  }
  ctx.putImageData(imageData, 0, 0);
  return true;
}

function hideImageOverlay() {
  if (imageState.renderTimer) {
    window.clearTimeout(imageState.renderTimer);
    imageState.renderTimer = null;
  }
  el.imageOverlay.hidden = true;
  el.imageOverlayImg.src = "";
  el.imageOverlayImg.hidden = false;
  el.imageOverlayCanvas.hidden = true;
  el.imageOverlay.style.removeProperty("--image-content-width");
  el.imageOverlay.style.removeProperty("--image-content-height");
  imageState.topic = null;
  imageState.dataB64 = "";
  imageState.renderMode = "";
  imageState.manualSize = false;
  imageState.sizedTopic = null;
  imageState.closedByUser = false;
}

function findVisualizedImageTopic() {
  const active = state.topics.get(state.activeTopic);
  if (active?.visualize && isImageType(active.type || active.msg_type) && active.imageData) {
    return active;
  }
  return [...state.topics.values()].find(
    (topic) =>
      topic.active !== false &&
      topic.visualize &&
      isImageType(topic.type || topic.msg_type) &&
      topic.imageData,
  );
}

function updateImageOverlay() {
  const active = findVisualizedImageTopic();
  if (!active) {
    hideImageOverlay();
    return;
  }

  const img = active.imageData;
  const needsUpdate =
    imageState.topic !== active.name ||
    imageState.dataB64 !== img.dataB64;
  if (!needsUpdate) return;

  const now = performance.now();
  if (imageState.dragActive || imageState.resizeActive) {
    scheduleImageOverlayRender(imageRenderIntervalMs);
    return;
  }
  const nextDelay = imageRenderIntervalMs - (now - imageState.lastRenderTime);
  if (nextDelay > 0) {
    scheduleImageOverlayRender(nextDelay);
    return;
  }

  renderImageOverlay(active, img, now);
}

function scheduleImageOverlayRender(delayMs) {
  if (imageState.renderTimer) return;
  imageState.renderTimer = window.setTimeout(() => {
    imageState.renderTimer = null;
    updateImageOverlay();
  }, Math.max(0, delayMs));
}

function overlayChromeHeight() {
  return el.imageOverlayHeader.offsetHeight;
}

function fitImageOverlayToImage(topicName, img) {
  if (
    imageState.manualSize &&
    imageState.sizedTopic === topicName &&
    imageState.sizedWidth === img.width &&
    imageState.sizedHeight === img.height
  ) {
    return;
  }

  const parent = el.imageOverlay.parentElement;
  const parentWidth = parent?.clientWidth || window.innerWidth;
  const parentHeight = parent?.clientHeight || window.innerHeight;
  const sourceWidth = Math.max(1, Number(img.width) || 1);
  const sourceHeight = Math.max(1, Number(img.height) || 1);
  const chromeHeight = overlayChromeHeight();
  const borderWidth = el.imageOverlay.offsetWidth - el.imageOverlay.clientWidth;
  const borderHeight = el.imageOverlay.offsetHeight - el.imageOverlay.clientHeight;
  const maxContentWidth = Math.max(160, parentWidth - 56);
  const maxContentHeight = Math.max(90, parentHeight - chromeHeight - 56);
  const scale = Math.min(maxContentWidth / sourceWidth, maxContentHeight / sourceHeight);
  const contentWidth = Math.max(160, Math.round(sourceWidth * scale));
  const contentHeight = Math.max(90, Math.round(contentWidth * (sourceHeight / sourceWidth)));
  const currentLeft = el.imageOverlay.offsetLeft;
  const currentTop = el.imageOverlay.offsetTop;

  el.imageOverlay.style.width = `${contentWidth + borderWidth}px`;
  el.imageOverlay.style.height = `${contentHeight + chromeHeight + borderHeight}px`;
  el.imageOverlay.style.right = "auto";

  el.imageOverlayBody.style.setProperty("--image-content-width", `${contentWidth}px`);
  el.imageOverlayBody.style.setProperty("--image-content-height", `${contentHeight}px`);
  el.imageOverlayBody.style.width = `${contentWidth}px`;
  el.imageOverlayBody.style.height = `${contentHeight}px`;

  const next = clampOverlayPosition(currentLeft, currentTop);
  el.imageOverlay.style.left = `${next.left}px`;
  el.imageOverlay.style.top = `${next.top}px`;

  imageState.sizedTopic = topicName;
  imageState.sizedWidth = img.width;
  imageState.sizedHeight = img.height;
  imageState.initialScale = scale;
  imageState.initialContentWidth = contentWidth;
  imageState.initialContentHeight = contentHeight;
}

function renderImageOverlay(active, img, renderTime) {
  const topicChanged = imageState.topic !== active.name;
  const sizeChanged =
    imageState.width !== img.width ||
    imageState.height !== img.height;

  imageState.closedByUser = false;
  imageState.topic = active.name;
  imageState.width = img.width;
  imageState.height = img.height;
  imageState.encoding = img.encoding;
  imageState.dataB64 = img.dataB64;
  imageState.dataLength = img.dataLength;
  imageState.lastRenderTime = renderTime;

  el.imageOverlay.hidden = false;
  el.imageOverlayTitle.textContent = active.name;

  const enc = (img.encoding || "").toLowerCase();
  if (enc === "jpeg" || enc === "jpg" || enc === "png") {
    const mime = enc === "jpg" ? "jpeg" : enc;
    if (imageState.renderMode !== "image") {
      el.imageOverlayCanvas.hidden = true;
      el.imageOverlayImg.hidden = false;
      imageState.renderMode = "image";
    }
    el.imageOverlayImg.src = `data:image/${mime};base64,${img.dataB64}`;
  } else {
    const bytes = decodeBase64(img.dataB64);
    const ok = renderRawImageToCanvas(
      bytes,
      img.width,
      img.height,
      enc,
      stepFromTopic(active),
      el.imageOverlayCanvas,
    );
    if (ok) {
      el.imageOverlayImg.hidden = true;
      el.imageOverlayCanvas.hidden = false;
      imageState.renderMode = "canvas";
    } else {
      if (imageState.renderMode !== "image") {
        el.imageOverlayCanvas.hidden = true;
        el.imageOverlayImg.hidden = false;
        imageState.renderMode = "image";
      }
      el.imageOverlayImg.src = `data:image/*;base64,${img.dataB64}`;
    }
  }

  if (topicChanged || sizeChanged) {
    // Use double rAF to guarantee the overlay is laid out after unhiding
    requestAnimationFrame(() => {
      requestAnimationFrame(() => {
        fitImageOverlayToImage(active.name, img);
        // Set explicit img dimensions matching the body
        el.imageOverlayImg.style.width = el.imageOverlayBody.style.width;
        el.imageOverlayImg.style.height = el.imageOverlayBody.style.height;
        el.imageOverlayCanvas.style.width = el.imageOverlayBody.style.width;
        el.imageOverlayCanvas.style.height = el.imageOverlayBody.style.height;
      });
    });
  }

  if (active.imageTimes && active.imageTimes.length > 2) {
    const span =
      active.imageTimes[active.imageTimes.length - 1] - active.imageTimes[0];
    imageState.rate =
      span > 0
        ? ((active.imageTimes.length - 1) * 1000) / span
        : 0;
  }
}

function stepFromTopic(topic) {
  return (topic.latest && topic.latest.step) || 0;
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
  updateDisplayForm();
  updateResolvedMessageType();
  el.topicInput.focus();
}

function closeModal() {
  el.displayModal.hidden = true;
}

function addTopicFromForm() {
  const topic = el.topicInput.value.trim();
  const msgType = el.typeInput.value;
  if (msgType === "Robot") {
    const name = topic || "Robot";
    const robot = {
      id: name.replaceAll(/[^A-Za-z0-9_.-]/g, "_"),
      name,
      display_type: "Robot",
      urdf_path: el.urdfPathInput.value.trim(),
      visible: true,
    };
    if (sendCommand({ action: "add_robot", robot })) {
      closeModal();
    }
    return;
  }
  if (msgType === "RobotState") {
    if (!topic) {
      addEvent("error", "robot_state topic is required");
      return;
    }
    const robotId = el.targetRobotInput.value;
    if (!robotId) {
      addEvent("error", "select a Robot display before adding RobotState");
      return;
    }
    const robotStateDisplay = {
      id: topic.replaceAll(/[^A-Za-z0-9_.-]/g, "_"),
      name: topic,
      topic,
      target_robot_id: robotId,
      msg_type: el.messageTypeInput.value || "RobotState",
      domain: Number(el.domainInput.value || state.config.domain || 1),
      queue: Number(el.queueInput.value || state.config.queue || 10),
      transport: el.transportInput.value || state.config.transport || "socket",
      poll: Number(el.pollInput.value || 30) / 1000,
      extra: el.extraInput.value || "",
    };
    if (sendCommand({ action: "add_robot_state", robot_state: robotStateDisplay })) {
      closeModal();
    }
    return;
  }
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

function xmlChildrenByName(node, name) {
  return Array.from(node.children || []).filter((child) => child.localName === name);
}

function xmlFirstChildByName(node, name) {
  return xmlChildrenByName(node, name)[0] || null;
}

function parseNumberList(value, fallback, expectedLength = 3) {
  if (!value) return [...fallback];
  const numbers = String(value)
    .trim()
    .split(/\s+/)
    .map((part) => Number(part));
  if (numbers.length !== expectedLength || numbers.some((number) => !Number.isFinite(number))) {
    return [...fallback];
  }
  return numbers;
}

function matrixFromOrigin(xyz, rpy) {
  const position = new THREE.Vector3(xyz[0], xyz[1], xyz[2]);
  const quaternion = new THREE.Quaternion().setFromEuler(new THREE.Euler(rpy[0], rpy[1], rpy[2], "XYZ"));
  return new THREE.Matrix4().compose(position, quaternion, new THREE.Vector3(1, 1, 1));
}

function matrixPosition(matrix) {
  return new THREE.Vector3().setFromMatrixPosition(matrix);
}

function parseUrdf(text, filename) {
  const doc = new DOMParser().parseFromString(text, "application/xml");
  const parseError = doc.querySelector("parsererror");
  if (parseError) {
    throw new Error(parseError.textContent.trim().split("\n")[0] || "invalid XML");
  }

  const robot = doc.documentElement?.localName === "robot" ? doc.documentElement : doc.querySelector("robot");
  if (!robot) throw new Error("missing <robot> root");

  const warnings = [];
  const linksByName = new Map();
  for (const linkEl of xmlChildrenByName(robot, "link")) {
    const name = linkEl.getAttribute("name")?.trim();
    if (!name) {
      warnings.push("ignored unnamed link");
      continue;
    }
    linksByName.set(name, { name });
  }

  const joints = [];
  const childLinkNames = new Set();
  const jointsByParent = new Map();
  for (const jointEl of xmlChildrenByName(robot, "joint")) {
    const name = jointEl.getAttribute("name")?.trim() || `joint_${joints.length + 1}`;
    const type = jointEl.getAttribute("type")?.trim() || "fixed";
    const parent = xmlFirstChildByName(jointEl, "parent")?.getAttribute("link")?.trim() || "";
    const child = xmlFirstChildByName(jointEl, "child")?.getAttribute("link")?.trim() || "";
    const originEl = xmlFirstChildByName(jointEl, "origin");
    const xyz = parseNumberList(originEl?.getAttribute("xyz"), [0, 0, 0]);
    const rpy = parseNumberList(originEl?.getAttribute("rpy"), [0, 0, 0]);
    const axis = parseNumberList(xmlFirstChildByName(jointEl, "axis")?.getAttribute("xyz"), [1, 0, 0]);
    const axisVector = new THREE.Vector3(axis[0], axis[1], axis[2]);
    if (axisVector.lengthSq() < 1e-12) axisVector.set(1, 0, 0);
    axisVector.normalize();

    if (!parent || !child) {
      warnings.push(`ignored joint ${name}: missing parent or child`);
      continue;
    }
    if (!linksByName.has(parent)) warnings.push(`joint ${name}: missing parent link ${parent}`);
    if (!linksByName.has(child)) warnings.push(`joint ${name}: missing child link ${child}`);

    const joint = {
      name,
      type,
      parent,
      child,
      xyz,
      rpy,
      axis: axisVector.toArray(),
      originMatrix: matrixFromOrigin(xyz, rpy),
    };
    joints.push(joint);
    childLinkNames.add(child);
    if (!jointsByParent.has(parent)) jointsByParent.set(parent, []);
    jointsByParent.get(parent).push(joint);
  }

  if (linksByName.size === 0) throw new Error("URDF contains no links");

  const roots = [...linksByName.keys()].filter((name) => !childLinkNames.has(name));
  if (roots.length === 0) {
    roots.push(linksByName.keys().next().value);
    warnings.push("no root link found; using first link");
  }

  const linkPoses = new Map();
  const jointPoses = new Map();
  const visiting = new Set();
  const visited = new Set();

  function visitLink(linkName, parentPose) {
    if (visiting.has(linkName)) {
      warnings.push(`cycle detected at link ${linkName}`);
      return;
    }
    if (visited.has(linkName)) return;
    visiting.add(linkName);
    linkPoses.set(linkName, parentPose.clone());
    visited.add(linkName);

    for (const joint of jointsByParent.get(linkName) || []) {
      const childPose = parentPose.clone().multiply(joint.originMatrix);
      jointPoses.set(joint.name, childPose.clone());
      if (linksByName.has(joint.child)) visitLink(joint.child, childPose);
    }
    visiting.delete(linkName);
  }

  roots.forEach((root) => visitLink(root, new THREE.Matrix4()));
  for (const linkName of linksByName.keys()) {
    if (!linkPoses.has(linkName)) {
      warnings.push(`link ${linkName} is disconnected from root`);
      linkPoses.set(linkName, new THREE.Matrix4());
    }
  }

  return {
    id: robotState.nextId++,
    name: robot.getAttribute("name")?.trim() || filename || "URDF Robot",
    filename,
    links: [...linksByName.values()].sort((a, b) => a.name.localeCompare(b.name)),
    joints,
    roots,
    linkPoses,
    jointPoses,
    warnings,
  };
}

function makeLabel(text, color, scale = 0.65) {
  const canvas = document.createElement("canvas");
  canvas.width = 96;
  canvas.height = 96;
  const ctx = canvas.getContext("2d");
  ctx.clearRect(0, 0, canvas.width, canvas.height);
  ctx.font = "700 56px Inter, Arial, sans-serif";
  ctx.textAlign = "center";
  ctx.textBaseline = "middle";
  ctx.fillStyle = color;
  ctx.fillText(text, canvas.width / 2, canvas.height / 2);

  const texture = new THREE.CanvasTexture(canvas);
  texture.needsUpdate = true;
  const material = new THREE.SpriteMaterial({
    map: texture,
    transparent: true,
    depthTest: false,
    depthWrite: false,
  });
  const sprite = new THREE.Sprite(material);
  sprite.scale.set(scale, scale, 1);
  return sprite;
}

function makeAxisLine(points, color, thickness = 2) {
  return new THREE.Line(
    new THREE.BufferGeometry().setFromPoints(points),
    new THREE.LineBasicMaterial({ color, linewidth: thickness }),
  );
}

function makeAxisGizmo(size = 1, labelScale = 0.34, thickness = 2) {
  const group = new THREE.Group();
  group.add(makeAxisLine([new THREE.Vector3(0, 0, 0), new THREE.Vector3(size, 0, 0)], axisColors.x, thickness));
  group.add(makeAxisLine([new THREE.Vector3(0, 0, 0), new THREE.Vector3(0, size, 0)], axisColors.y, thickness));
  group.add(makeAxisLine([new THREE.Vector3(0, 0, 0), new THREE.Vector3(0, 0, size)], axisColors.z, thickness));

  const xLabel = makeLabel("X", "#ef4444", labelScale);
  xLabel.position.set(size * 1.16, 0, 0);
  const yLabel = makeLabel("Y", "#22c55e", labelScale);
  yLabel.position.set(0, size * 1.16, 0);
  const zLabel = makeLabel("Z", "#3b82f6", labelScale);
  zLabel.position.set(0, 0, size * 1.16);
  group.add(xLabel, yLabel, zLabel);

  return group;
}

function makeFrameAxes(size = 0.32, thickness = 2) {
  const group = new THREE.Group();
  group.add(makeAxisLine([new THREE.Vector3(0, 0, 0), new THREE.Vector3(size, 0, 0)], axisColors.x, thickness));
  group.add(makeAxisLine([new THREE.Vector3(0, 0, 0), new THREE.Vector3(0, size, 0)], axisColors.y, thickness));
  group.add(makeAxisLine([new THREE.Vector3(0, 0, 0), new THREE.Vector3(0, 0, size)], axisColors.z, thickness));
  return group;
}

function jointColor(type) {
  if (type === "fixed") return 0x64748b;
  if (type === "prismatic") return 0x22c55e;
  if (type === "continuous" || type === "revolute") return 0xf59e0b;
  return 0x38bdf8;
}

function createRobotGroup(model) {
  const group = new THREE.Group();
  group.name = `urdf:${model.name}`;
  group.userData.robotModelId = model.id;

  const linkFrames = new THREE.Group();
  linkFrames.name = "link_frames";
  for (const link of model.links) {
    const pose = model.linkPoses.get(link.name);
    if (!pose) continue;
    const frame = makeFrameAxes(0.32, 2);
    frame.name = `link_frame:${link.name}`;
    frame.matrixAutoUpdate = false;
    frame.matrix.copy(pose);
    linkFrames.add(frame);
  }
  group.add(linkFrames);

  const segmentPoints = [];
  for (const joint of model.joints) {
    const parentPose = model.linkPoses.get(joint.parent);
    const jointPose = model.jointPoses.get(joint.name);
    if (!parentPose || !jointPose) continue;
    segmentPoints.push(matrixPosition(parentPose), matrixPosition(jointPose));
  }
  if (segmentPoints.length > 0) {
    const linkLines = new THREE.LineSegments(
      new THREE.BufferGeometry().setFromPoints(segmentPoints),
      new THREE.LineBasicMaterial({ color: 0x0f172a, transparent: true, opacity: 0.72 }),
    );
    linkLines.name = "link_segments";
    group.add(linkLines);
  }

  const markerGeometry = new THREE.SphereGeometry(0.075, 16, 10);
  for (const joint of model.joints) {
    const jointPose = model.jointPoses.get(joint.name);
    if (!jointPose) continue;
    const position = matrixPosition(jointPose);
    const color = jointColor(joint.type);
    const marker = new THREE.Mesh(
      markerGeometry.clone(),
      new THREE.MeshStandardMaterial({
        color,
        emissive: color,
        emissiveIntensity: 0.12,
        roughness: 0.48,
        metalness: 0.04,
      }),
    );
    marker.name = `joint:${joint.name}`;
    marker.position.copy(position);
    group.add(marker);

    const rotation = new THREE.Quaternion().setFromRotationMatrix(jointPose);
    const axis = new THREE.Vector3(joint.axis[0], joint.axis[1], joint.axis[2]).applyQuaternion(rotation).normalize();
    const axisLine = makeAxisLine(
      [
        position.clone().addScaledVector(axis, -0.18),
        position.clone().addScaledVector(axis, 0.18),
      ],
      color,
      2,
    );
    axisLine.name = `joint_axis:${joint.name}`;
    group.add(axisLine);
  }
  markerGeometry.dispose();

  return group;
}

function parseUrdfRobot(urdfText, fileName = "") {
  const model = parseUrdf(urdfText, fileName);
  model.urdf = urdfText;
  model.fileName = fileName;
  model.visible = true;
  return model;
}

function buildRobotObject(robot) {
  return createRobotGroup(robot);
}

function initScene() {
  THREE.Object3D.DEFAULT_UP.set(0, 0, 1);
  const scene = new THREE.Scene();
  scene.background = new THREE.Color(sceneBackgroundColor);

  const camera = new THREE.PerspectiveCamera(55, 1, 0.05, 1000);
  camera.up.set(0, 0, 1);
  camera.position.copy(defaultCameraPosition);

  const renderer = new THREE.WebGLRenderer({
    canvas: el.sceneCanvas,
    antialias: true,
    alpha: false,
    powerPreference: "high-performance",
  });
  renderer.setClearColor(sceneBackgroundColor, 1);
  renderer.setPixelRatio(Math.min(window.devicePixelRatio || 1, 2));
  renderer.autoClear = false;

  const controls = new OrbitControls(camera, renderer.domElement);
  controls.enableDamping = true;
  controls.dampingFactor = 0.08;
  controls.screenSpacePanning = false;
  controls.minDistance = 3;
  controls.maxDistance = 140;
  controls.maxPolarAngle = Math.PI * 0.48;
  controls.mouseButtons = {
    LEFT: THREE.MOUSE.ROTATE,
    MIDDLE: THREE.MOUSE.DOLLY,
    RIGHT: THREE.MOUSE.PAN,
  };
  controls.target.set(0, 0, 0);
  controls.update();

  const ambient = new THREE.AmbientLight(0xffffff, 0.58);
  scene.add(ambient);
  const keyLight = new THREE.DirectionalLight(0xffffff, 0.52);
  keyLight.position.set(8, -7, 12);
  scene.add(keyLight);

  const grid = new THREE.GridHelper(80, 80, 0x94a3b8, 0xcbd5e1);
  grid.rotation.x = gridPlaneRotation;
  grid.material.transparent = true;
  grid.material.opacity = 0.66;
  scene.add(grid);

  const axisScene = new THREE.Scene();
  const axisCamera = new THREE.PerspectiveCamera(45, 1, 0.1, 20);
  axisCamera.up.set(0, 0, 1);
  axisScene.add(
    makeAxisGizmo(
      referenceAxisConfig.axisSize,
      referenceAxisConfig.labelScale,
      referenceAxisConfig.axisThickness,
    ),
  );

  sceneState.renderer = renderer;
  sceneState.scene = scene;
  sceneState.camera = camera;
  sceneState.controls = controls;
  sceneState.axisScene = axisScene;
  sceneState.axisCamera = axisCamera;
  sceneState.grid = grid;

  renderer.domElement.addEventListener("contextmenu", (event) => event.preventDefault());
  controls.addEventListener("start", () => {
    renderer.domElement.classList.add("orbiting");
    requestSceneRender(sceneInteractionRenderMs);
  });
  controls.addEventListener("change", () => requestSceneRender(sceneInteractionRenderMs));
  controls.addEventListener("end", () => {
    renderer.domElement.classList.remove("orbiting", "panning");
    requestSceneRender(sceneInteractionRenderMs);
  });
  renderer.domElement.addEventListener("pointerdown", (event) => {
    renderer.domElement.classList.toggle("panning", event.button === 2);
    requestSceneRender(sceneInteractionRenderMs);
  });
}

function resizeRenderer() {
  const { renderer, camera } = sceneState;
  if (!renderer || !camera) return;
  const rect = el.sceneCanvas.getBoundingClientRect();
  const width = Math.max(1, Math.floor(rect.width));
  const height = Math.max(1, Math.floor(rect.height));
  const current = new THREE.Vector2();
  renderer.getSize(current);
  if (current.x !== width || current.y !== height) {
    renderer.setSize(width, height, false);
    camera.aspect = width / height;
    camera.updateProjectionMatrix();
  }
}

function topicColor(topic) {
  const names = [...state.topics.keys()].sort();
  const index = Math.max(0, names.indexOf(topic.name));
  return topic.name === state.activeTopic ? 0xfacc15 : topicPalette[index % topicPalette.length];
}

function disposeObject(object) {
  object.geometry?.dispose();
  if (Array.isArray(object.material)) {
    object.material.forEach((material) => {
      material.map?.dispose();
      material.dispose();
    });
  } else {
    object.material?.map?.dispose();
    object.material?.dispose();
  }
}

function disposeObjectTree(object) {
  object.traverse((child) => disposeObject(child));
}

function removeTopicObject(name) {
  const objectSet = sceneState.topicObjects.get(name);
  if (!objectSet || !sceneState.scene) return;
  const topic = state.topics.get(name);
  sceneState.scene.remove(objectSet.line, objectSet.head);
  if (objectSet.points) sceneState.scene.remove(objectSet.points);
  if (objectSet.mapGroup) sceneState.scene.remove(objectSet.mapGroup);
  disposeObject(objectSet.line);
  disposeObject(objectSet.head);
  if (objectSet.points) disposeObject(objectSet.points);
  if (objectSet.mapGroup) disposeObjectTree(objectSet.mapGroup);
  if (topic?.pointCloudMap) {
    for (const [chunkKey, chunk] of topic.pointCloudMap.chunks.entries()) {
      chunk.points = null;
      chunk.dirty = true;
      topic.pointCloudMap.dirtyChunks.add(chunkKey);
    }
  }
  sceneState.topicObjects.delete(name);
}

function removeRobotObject(robotId) {
  if (!sceneState.scene) return;
  const group = sceneState.robotGroups.get(robotId);
  if (!group) return;
  sceneState.scene.remove(group);
  disposeObjectTree(group);
  sceneState.robotGroups.delete(robotId);
}

function syncRobotObject() {
  const { scene } = sceneState;
  if (!scene) return;
  for (const robotId of [...sceneState.robotGroups.keys()]) {
    const robot = state.robotDisplays.get(robotId);
    if (!robot || !robot.urdf || robot.visible === false) {
      removeRobotObject(robotId);
    }
  }
  for (const robot of state.robotDisplays.values()) {
    if (!robot.urdf || robot.visible === false) continue;
    let group = sceneState.robotGroups.get(robot.id);
    if (!group || group.userData.robotModelId !== robot.id) {
      removeRobotObject(robot.id);
      group = createRobotGroup(robot);
      sceneState.robotGroups.set(robot.id, group);
      scene.add(group);
    }
    group.visible = robot.visible !== false;
  }
}

function frameRobotModel(model) {
  if (!model || !sceneState.camera || !sceneState.controls) return;
  const points = [...model.linkPoses.values()].map((pose) => matrixPosition(pose));
  if (points.length === 0) return;
  const box = new THREE.Box3().setFromPoints(points);
  const center = box.getCenter(new THREE.Vector3());
  const size = box.getSize(new THREE.Vector3());
  const radius = Math.max(1.2, size.length() * 0.62);
  const direction = sceneState.camera.position.clone().sub(sceneState.controls.target);
  if (direction.lengthSq() < 1e-6) direction.copy(defaultCameraPosition);
  direction.normalize();
  sceneState.controls.target.copy(center);
  sceneState.camera.position.copy(center).addScaledVector(direction, Math.min(140, Math.max(4, radius * 3.4)));
  sceneState.camera.updateProjectionMatrix();
  sceneState.controls.update();
  renderToolbar();
  requestSceneRender(sceneInteractionRenderMs);
}

function syncTopicObjects() {
  const { scene } = sceneState;
  if (!scene) return;

  for (const name of [...sceneState.topicObjects.keys()]) {
    const topic = state.topics.get(name);
    const hasPoseTrail = topic?.poseTrail.length > 0;
    const hasPointCloud = Boolean(topic?.pointCloud);
    if (!topic || topic.active === false || !topic.visualize || (!hasPoseTrail && !hasPointCloud)) {
      removeTopicObject(name);
    }
  }

  for (const topic of state.topics.values()) {
    const hasPoseTrail = topic.poseTrail.length > 0;
    const hasPointCloud = Boolean(topic.pointCloud);
    if (topic.active === false || !topic.visualize || (!hasPoseTrail && !hasPointCloud)) continue;
    let objectSet = sceneState.topicObjects.get(topic.name);
    if (!objectSet) {
      const line = new THREE.Line(
        new THREE.BufferGeometry(),
        new THREE.LineBasicMaterial({ color: topicColor(topic), linewidth: 2 }),
      );
      const head = new THREE.Mesh(
        new THREE.SphereGeometry(0.18, 24, 16),
        new THREE.MeshStandardMaterial({
          color: topicColor(topic),
          emissive: topicColor(topic),
          emissiveIntensity: 0.22,
          roughness: 0.5,
          metalness: 0.05,
        }),
      );
      scene.add(line, head);
      objectSet = {
        line,
        head,
        points: null,
        mapGroup: null,
        lastLength: 0,
        lastPointCloudSample: null,
        lastPointCloudRenderStep: 1,
        lastPointCloudRenderTime: 0,
      };
      sceneState.topicObjects.set(topic.name, objectSet);
    }

    const color = topicColor(topic);
    objectSet.line.material.color.setHex(color);
    objectSet.head.material.color.setHex(color);
    objectSet.head.material.emissive.setHex(color);
    objectSet.line.visible = hasPoseTrail;
    objectSet.head.visible = hasPoseTrail;

    if (hasPoseTrail && objectSet.lastLength !== topic.poseTrail.length) {
      const points = topic.poseTrail.map((pose) => new THREE.Vector3(pose.x, pose.y, pose.z));
      objectSet.line.geometry.dispose();
      objectSet.line.geometry = new THREE.BufferGeometry().setFromPoints(points);
      objectSet.lastLength = topic.poseTrail.length;
    }

    if (hasPoseTrail) {
      const latest = topic.poseTrail[topic.poseTrail.length - 1];
      objectSet.head.position.set(latest.x, latest.y, latest.z);
    }

    const renderBudget = hasPointCloud ? tunePointCloudRenderBudget(topic) : null;
    if (
      hasPointCloud &&
      (
        objectSet.lastPointCloudSample !== topic.pointCloud.sampleId ||
        objectSet.lastPointCloudRenderStep !== renderBudget.latestStep
      )
    ) {
      const now = performance.now();
      const pointCloudElapsed = now - objectSet.lastPointCloudRenderTime;
      if (objectSet.points && pointCloudElapsed < minPointCloudFrameMs) {
        requestPointCloudRender();
        continue;
      }
      if (objectSet.points) {
        scene.remove(objectSet.points);
        disposeObject(objectSet.points);
      }
      const geometry = new THREE.BufferGeometry();
      geometry.setAttribute(
        "position",
        new THREE.BufferAttribute(
          sampledFloat32Array(topic.pointCloud.positions, 3, renderBudget.latestStep),
          3,
        ),
      );
      const materialOptions = {
        size: pointCloudConfig.pointSize,
        sizeAttenuation: true,
      };
      if (topic.pointCloud.colors) {
        geometry.setAttribute(
          "color",
          new THREE.BufferAttribute(
            sampledFloat32Array(topic.pointCloud.colors, 3, renderBudget.latestStep),
            3,
          ),
        );
        materialOptions.vertexColors = true;
      } else {
        materialOptions.color = pointCloudConfig.fallbackColor;
      }
      objectSet.points = new THREE.Points(geometry, new THREE.PointsMaterial(materialOptions));
      scene.add(objectSet.points);
      objectSet.lastPointCloudSample = topic.pointCloud.sampleId;
      objectSet.lastPointCloudRenderStep = renderBudget.latestStep;
      objectSet.lastPointCloudRenderTime = now;
    } else if (!hasPointCloud && objectSet.points) {
      scene.remove(objectSet.points);
      disposeObject(objectSet.points);
      objectSet.points = null;
      objectSet.lastPointCloudSample = null;
      objectSet.lastPointCloudRenderStep = 1;
      objectSet.lastPointCloudRenderTime = 0;
    }
    syncPointCloudMapObjects(topic, objectSet);
  }
}

function syncPointCloudMapObjects(topic, objectSet) {
  const map = topic.pointCloudMap;
  if (!map || !sceneState.scene) return;
  if (!objectSet.mapGroup) {
    objectSet.mapGroup = new THREE.Group();
    objectSet.mapGroup.name = `pointcloud_map:${topic.name}`;
    sceneState.scene.add(objectSet.mapGroup);
  }
  objectSet.mapGroup.visible = topic.visualize !== false;
  if (map.dirtyChunks.size === 0) return;

  const now = performance.now();
  const renderBudget = ensurePointCloudRenderBudget(topic);
  const elapsed = now - map.lastGpuUpdate;
  if (map.lastGpuUpdate > 0 && elapsed < pointCloudMapConfig.gpuUpdateIntervalMs) {
    requestPointCloudMapRender();
    return;
  }

  let updated = 0;
  for (const chunkKey of [...map.dirtyChunks]) {
    const chunk = map.chunks.get(chunkKey);
    map.dirtyChunks.delete(chunkKey);
    if (!chunk || chunk.positions.length === 0) continue;
    if (chunk.points) {
      objectSet.mapGroup.remove(chunk.points);
      disposeObject(chunk.points);
    }
    const geometry = new THREE.BufferGeometry();
    geometry.setAttribute(
      "position",
      new THREE.BufferAttribute(
        sampledFloat32Array(chunk.positions, 3, renderBudget.mapStep),
        3,
      ),
    );
    geometry.setAttribute(
      "color",
      new THREE.BufferAttribute(
        sampledFloat32Array(chunk.colors, 3, renderBudget.mapStep),
        3,
      ),
    );
    chunk.points = new THREE.Points(
      geometry,
      new THREE.PointsMaterial({
        size: pointCloudConfig.pointSize,
        sizeAttenuation: true,
        vertexColors: true,
      }),
    );
    chunk.points.name = `pointcloud_map_chunk:${topic.name}:${chunk.key}`;
    objectSet.mapGroup.add(chunk.points);
    chunk.dirty = false;
    updated += 1;
    if (updated >= pointCloudMapConfig.maxDirtyChunksPerFrame) break;
  }
  map.lastGpuUpdate = now;
  if (map.dirtyChunks.size > 0) requestPointCloudMapRender();
}

function renderAxisGizmo() {
  const { renderer, camera, controls, axisScene, axisCamera } = sceneState;
  if (!renderer || !camera || !controls || !axisScene || !axisCamera) return;

  const renderSize = renderer.getSize(new THREE.Vector2());
  const width = renderSize.x;
  const height = renderSize.y;
  const inset = referenceAxisConfig.viewportInset;
  const size = Math.min(
    referenceAxisConfig.viewportSize,
    Math.floor(width * 0.32),
    Math.floor(height * 0.32),
  );
  if (size < 64) return;

  const direction = camera.position.clone().sub(controls.target).normalize();
  axisCamera.position.copy(direction.multiplyScalar(referenceAxisConfig.cameraDistance));
  axisCamera.lookAt(0, 0, 0);

  renderer.clearDepth();
  renderer.setScissorTest(true);
  renderer.setViewport(width - size - inset, inset, size, size);
  renderer.setScissor(width - size - inset, inset, size, size);
  renderer.setClearColor(referenceAxisConfig.backgroundColor, referenceAxisConfig.backgroundOpacity);
  renderer.clearColor();
  renderer.render(axisScene, axisCamera);
  renderer.setClearColor(sceneBackgroundColor, 1);
  renderer.setScissorTest(false);
  renderer.setViewport(0, 0, width, height);
}

function updateSceneFpsMetric(renderTime) {
  sceneRenderState.frameTimes.push(renderTime);
  const cutoff = renderTime - sceneFpsWindowMs;
  while (
    sceneRenderState.frameTimes.length > 0 &&
    sceneRenderState.frameTimes[0] < cutoff
  ) {
    sceneRenderState.frameTimes.shift();
  }
  if (renderTime - sceneRenderState.lastFpsUpdate >= sceneFpsUpdateIntervalMs) {
    sceneRenderState.lastFpsUpdate = renderTime;
    const fps = sceneRenderState.frameTimes.length * (1000 / sceneFpsWindowMs);
    el.sceneFpsMetric.textContent = fps.toFixed(1);
  }
  if (sceneRenderState.fpsIdleTimer) {
    window.clearTimeout(sceneRenderState.fpsIdleTimer);
  }
  sceneRenderState.fpsIdleTimer = window.setTimeout(() => {
    sceneRenderState.frameTimes = [];
    el.sceneFpsMetric.textContent = "0.0";
  }, sceneFpsWindowMs + sceneFpsUpdateIntervalMs);
}

function renderScene() {
  const { renderer, scene, camera, controls } = sceneState;
  sceneRenderState.pending = false;
  if (!renderer || !scene || !camera || !controls) return;
  const renderTime = performance.now();
  resizeRenderer();
  syncTopicObjects();
  syncRobotObject();
  controls.update();
  renderer.clear();
  renderer.render(scene, camera);
  renderAxisGizmo();
  renderToolbar();
  sceneRenderState.lastRenderTime = renderTime;
  updateSceneFpsMetric(renderTime);
  // OrbitControls damping needs a few follow-up frames after input, but
  // the scene should not render forever while it is static.
  if (performance.now() < sceneRenderState.continuousUntil) {
    requestSceneRender();
  }
}

function queueSceneAnimationFrame() {
  sceneRenderState.timer = null;
  sceneRenderState.scheduledAt = 0;
  if (sceneRenderState.pending) return;
  sceneRenderState.pending = true;
  requestAnimationFrame(renderScene);
}

function requestSceneRender(continuousMs = 0, minFrameMs = minSceneFrameMs) {
  if (continuousMs > 0) {
    sceneRenderState.continuousUntil = Math.max(
      sceneRenderState.continuousUntil,
      performance.now() + continuousMs,
    );
  }
  const now = performance.now();
  const dueAt = sceneRenderState.lastRenderTime + minFrameMs;
  const delay = Math.max(0, dueAt - now);
  if (delay > 0) {
    const scheduledAt = now + delay;
    if (
      sceneRenderState.timer &&
      sceneRenderState.scheduledAt <= scheduledAt
    ) {
      return;
    }
    if (sceneRenderState.timer) window.clearTimeout(sceneRenderState.timer);
    sceneRenderState.scheduledAt = scheduledAt;
    sceneRenderState.timer = window.setTimeout(queueSceneAnimationFrame, delay);
    return;
  }
  if (sceneRenderState.timer) {
    window.clearTimeout(sceneRenderState.timer);
    sceneRenderState.timer = null;
    sceneRenderState.scheduledAt = 0;
  }
  // Coalesce multiple data/UI changes into one rAF so high-rate samples
  // do not schedule parallel WebGL renders.
  if (sceneRenderState.pending) return;
  queueSceneAnimationFrame();
}

function requestPointCloudRender() {
  requestSceneRender(0, minPointCloudFrameMs);
}

function requestPointCloudMapRender() {
  requestSceneRender(0, pointCloudMapConfig.gpuUpdateIntervalMs);
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

window.dzipcVisualizer = {
  state,
  sceneState,
  robotState,
  parseUrdfRobot,
  buildRobotObject,
  syncRobotObject,
  requestSceneRender,
};

window.addEventListener("resize", () => {
  renderToolbar();
  requestSceneRender();
});
setInterval(renderToolbar, 500);

// --- image overlay drag to move ---

function clampOverlayPosition(left, top) {
  const parent = el.imageOverlay.parentElement;
  const maxLeft = Math.max(0, (parent?.clientWidth || window.innerWidth) - el.imageOverlay.offsetWidth);
  const maxTop = Math.max(0, (parent?.clientHeight || window.innerHeight) - el.imageOverlay.offsetHeight);
  return {
    left: Math.min(Math.max(0, left), maxLeft),
    top: Math.min(Math.max(0, top), maxTop),
  };
}

el.imageOverlayHeader.addEventListener("pointerdown", (event) => {
  if (event.target === el.imageOverlayClose) return;
  imageState.dragActive = true;
  imageState.dragStartX = event.clientX;
  imageState.dragStartY = event.clientY;
  imageState.dragOrigLeft = el.imageOverlay.offsetLeft;
  imageState.dragOrigTop = el.imageOverlay.offsetTop;
  imageState.dragNextLeft = imageState.dragOrigLeft;
  imageState.dragNextTop = imageState.dragOrigTop;
  el.imageOverlay.style.right = "auto";
  el.imageOverlay.style.transition = "none";
  el.imageOverlay.classList.add("dragging");
  el.imageOverlayHeader.setPointerCapture(event.pointerId);
});

el.imageOverlayHeader.addEventListener("pointermove", (event) => {
  if (!imageState.dragActive) return;
  const dx = event.clientX - imageState.dragStartX;
  const dy = event.clientY - imageState.dragStartY;
  const next = clampOverlayPosition(imageState.dragOrigLeft + dx, imageState.dragOrigTop + dy);
  imageState.dragNextLeft = next.left;
  imageState.dragNextTop = next.top;
  el.imageOverlay.style.transform = `translate3d(${next.left - imageState.dragOrigLeft}px, ${next.top - imageState.dragOrigTop}px, 0)`;
});

function finishImageOverlayDrag(event) {
  if (!imageState.dragActive) return;
  imageState.dragActive = false;
  el.imageOverlay.style.left = `${imageState.dragNextLeft}px`;
  el.imageOverlay.style.top = `${imageState.dragNextTop}px`;
  el.imageOverlay.style.transform = "";
  el.imageOverlay.style.transition = "";
  el.imageOverlay.classList.remove("dragging");
  if (event?.pointerId !== undefined && el.imageOverlayHeader.hasPointerCapture(event.pointerId)) {
    el.imageOverlayHeader.releasePointerCapture(event.pointerId);
  }
  requestSceneRender();
  updateImageOverlay();
}

el.imageOverlayHeader.addEventListener("pointerup", finishImageOverlayDrag);
el.imageOverlayHeader.addEventListener("pointercancel", finishImageOverlayDrag);

el.imageOverlayClose.addEventListener("click", () => {
  const topic = state.topics.get(imageState.topic);
  if (topic) topic.visualize = false;
  hideImageOverlay();
  renderDisplays();
  renderToolbar();
  requestSceneRender();
});

// --- image overlay resize ---

el.imageResizeHandle.addEventListener("pointerdown", (event) => {
  imageState.resizeActive = true;
  imageState.manualSize = true;
  imageState.resizeStartX = event.clientX;
  imageState.resizeStartY = event.clientY;
  imageState.resizeOrigWidth = el.imageOverlay.offsetWidth;
  imageState.resizeOrigHeight = el.imageOverlay.offsetHeight;
  imageState.resizeOrigContentWidth = Math.max(1, el.imageOverlayBody.offsetWidth);
  imageState.resizeOrigContentHeight = Math.max(1, el.imageOverlayBody.offsetHeight);
  el.imageOverlay.style.transition = "none";
  el.imageResizeHandle.setPointerCapture(event.pointerId);
  event.stopPropagation();
});

el.imageResizeHandle.addEventListener("pointermove", (event) => {
  if (!imageState.resizeActive) return;
  const dx = event.clientX - imageState.resizeStartX;
  const dy = event.clientY - imageState.resizeStartY;
  const parent = el.imageOverlay.parentElement;
  const chromeHeight = overlayChromeHeight();
  const borderWidth = el.imageOverlay.offsetWidth - el.imageOverlay.clientWidth;
  const borderHeight = el.imageOverlay.offsetHeight - el.imageOverlay.clientHeight;
  const maxContentWidth = Math.max(160, (parent?.clientWidth || window.innerWidth) - el.imageOverlay.offsetLeft - borderWidth);
  const maxContentHeight = Math.max(90, (parent?.clientHeight || window.innerHeight) - el.imageOverlay.offsetTop - chromeHeight - borderHeight);
  const widthScale = (imageState.resizeOrigContentWidth + dx) / imageState.resizeOrigContentWidth;
  const heightScale = (imageState.resizeOrigContentHeight + dy) / imageState.resizeOrigContentHeight;
  const minScale = Math.max(160 / imageState.resizeOrigContentWidth, 90 / imageState.resizeOrigContentHeight);
  const maxScale = Math.min(maxContentWidth / imageState.resizeOrigContentWidth, maxContentHeight / imageState.resizeOrigContentHeight);
  const scale = Math.min(maxScale, Math.max(minScale, widthScale, heightScale));
  const contentWidth = Math.round(imageState.resizeOrigContentWidth * scale);
  const contentHeight = Math.round(contentWidth * (imageState.resizeOrigContentHeight / imageState.resizeOrigContentWidth));
  el.imageOverlay.style.width = `${contentWidth + borderWidth}px`;
  el.imageOverlay.style.height = `${contentHeight + chromeHeight + borderHeight}px`;

  el.imageOverlayBody.style.setProperty("--image-content-width", `${contentWidth}px`);
  el.imageOverlayBody.style.setProperty("--image-content-height", `${contentHeight}px`);
  el.imageOverlayBody.style.width = `${contentWidth}px`;
  el.imageOverlayBody.style.height = `${contentHeight}px`;
  el.imageOverlayImg.style.width = `${contentWidth}px`;
  el.imageOverlayImg.style.height = `${contentHeight}px`;
  el.imageOverlayCanvas.style.width = `${contentWidth}px`;
  el.imageOverlayCanvas.style.height = `${contentHeight}px`;
  requestSceneRender();
});

el.imageResizeHandle.addEventListener("pointerup", () => {
  imageState.resizeActive = false;
  el.imageOverlay.style.transition = "";
  requestSceneRender();
  updateImageOverlay();
});

el.imageResizeHandle.addEventListener("pointerleave", () => {
  imageState.resizeActive = false;
  el.imageOverlay.style.transition = "";
  requestSceneRender();
  updateImageOverlay();
});
setConfig(state.config);
setMessageTypes(state.messageTypes);
updateDisplayForm();
closeModal();
initScene();
connect();
requestSceneRender();
