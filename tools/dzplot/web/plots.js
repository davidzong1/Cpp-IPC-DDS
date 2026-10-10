/** 多曲线绘图窗口与递归分裂布局。数据缓冲按字段共享，显示选择按窗口独立。 */
export const seriesKey = (topic, field) => `${topic}\x00${field}`;

export function seriesColor(topic, field, used = new Set()) {
  const palette = ["#347aba", "#9b669d", "#c48639", "#519377", "#be5e70", "#677eac"];
  let hash = 0;
  for (const char of `${topic} / ${field}`) hash = (hash * 31 + char.charCodeAt(0)) >>> 0;
  for (let i = 0; i < palette.length; i++) {
    const color = palette[(hash + i) % palette.length];
    if (!used.has(color)) return color;
  }
  let index = used.size, color;
  do { color = `hsl(${((index++ * 137.508) % 360).toFixed(2)} 58% 43%)`; } while (used.has(color));
  return color;
}

class PlotWindow {
  constructor(id, number, state, actions) {
    this.id = id;
    this.number = number;
    this.state = state;
    this.series = new Map();
    this.dirty = true;
    this.el = document.createElement("section");
    this.el.className = "plot-window";
    this.el.dataset.plotId = id;
    this.el.tabIndex = 0;
    this.el.setAttribute("role", "region");
    this.el.setAttribute("aria-label", `绘图窗口 ${number}`);
    this.el.innerHTML = `
      <header class="plot-window-head">
        <span class="plot-window-title">窗口 ${number}</span><span class="plot-series-count">0 条曲线</span>
        <span class="header-spacer"></span>
        <button class="plot-split-right icon-button" type="button" title="左右分裂此窗口" aria-label="左右分裂窗口 ${number}">◫</button>
        <button class="plot-split-down icon-button" type="button" title="上下分裂此窗口" aria-label="上下分裂窗口 ${number}">⬒</button>
        <button class="plot-window-close icon-button" type="button" title="关闭此窗口" aria-label="关闭窗口 ${number}">×</button>
      </header>
      <div class="plot-legend" aria-label="窗口 ${number} 的曲线"></div>
      <div class="plot-surface">
        <canvas class="chart-canvas" aria-label="窗口 ${number} 的时间序列"></canvas>
        <div class="plot-empty"><span>将话题或字段拖到这里</span><small>也可先选中此窗口，再勾选左侧字段</small></div>
      </div>`;
    this.canvas = this.el.querySelector("canvas");
    this.ctx = this.canvas.getContext("2d");
    this.surface = this.el.querySelector(".plot-surface");
    this.legend = this.el.querySelector(".plot-legend");
    this.el.addEventListener("pointerdown", () => actions.select(id));
    this.el.addEventListener("focusin", () => actions.select(id));
    this.el.querySelector(".plot-split-right").onclick = () => actions.split("row", id);
    this.el.querySelector(".plot-split-down").onclick = () => actions.split("column", id);
    this.el.querySelector(".plot-window-close").onclick = () => actions.close(id);
    this.el.ondragover = (event) => {
      if (![...event.dataTransfer.types].some((type) => type === "application/dzplot-field" || type === "application/dzplot-topic")) return;
      event.preventDefault();
      event.dataTransfer.dropEffect = "copy";
      this.el.classList.add("drag-over");
    };
    this.el.ondragleave = (event) => {
      if (!this.el.contains(event.relatedTarget)) this.el.classList.remove("drag-over");
    };
    this.el.ondrop = (event) => {
      event.preventDefault();
      this.el.classList.remove("drag-over");
      const raw = event.dataTransfer.getData("application/dzplot-field") || event.dataTransfer.getData("application/dzplot-topic");
      try { actions.drop(JSON.parse(raw), id); } catch {}
    };
    this.resizeObserver = new ResizeObserver(() => { this.markDirty(); this._draw(); });
    this.resizeObserver.observe(this.surface);
    this.removeSeries = (topic, field) => actions.remove(topic, field, id);
  }

  updateLegend() {
    this.legend.replaceChildren();
    this.el.querySelector(".plot-series-count").textContent = `${this.series.size} 条曲线`;
    this.el.querySelector(".plot-empty").hidden = this.series.size > 0;
    this.canvas.setAttribute("aria-label", `窗口 ${this.number} 的时间序列，${this.series.size} 条曲线`);
    for (const item of this.series.values()) {
      const chip = document.createElement("div");
      chip.className = "plot-series";
      chip.dataset.topic = item.topic;
      chip.dataset.field = item.field;
      chip.draggable = true;
      const color = document.createElement("i");
      color.className = "series-color";
      color.style.background = item.color;
      const name = document.createElement("span");
      name.textContent = `${item.topic} / ${item.field}`;
      name.title = name.textContent;
      const close = document.createElement("button");
      close.className = "plot-series-remove";
      close.type = "button";
      close.textContent = "×";
      close.setAttribute("aria-label", `从窗口 ${this.number} 移除 ${name.textContent}`);
      close.onclick = () => this.removeSeries(item.topic, item.field);
      chip.ondragstart = (event) => {
        event.dataTransfer.setData("application/dzplot-field", JSON.stringify({ topic: item.topic, field: item.field }));
        event.dataTransfer.effectAllowed = "copy";
      };
      chip.append(color, name, close);
      this.legend.append(chip);
    }
    this.markDirty();
  }

  markDirty() { this.dirty = true; }

  _draw() {
    if (!this.el.getClientRects().length) return;
    const rect = this.surface.getBoundingClientRect();
    const width = Math.floor(rect.width), height = Math.floor(rect.height);
    if (width < 1 || height < 1) return;
    const dpr = Math.min(window.devicePixelRatio || 1, 2);
    if (this._width !== width || this._height !== height || this._dpr !== dpr) {
      this.canvas.width = Math.round(width * dpr);
      this.canvas.height = Math.round(height * dpr);
      this._width = width; this._height = height; this._dpr = dpr;
    }
    const ctx = this.ctx;
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, width, height);
    ctx.fillStyle = "#ffffff";
    ctx.fillRect(0, 0, width, height);
    if (width < 100 || height < 65) return;

    let xMin = Infinity, xMax = -Infinity, yMin = Infinity, yMax = -Infinity;
    const data = [];
    for (const [key, item] of this.series) {
      const buffer = this.state.chartData[key];
      if (!buffer?.times.length) continue;
      data.push({ ...item, ...buffer });
      for (let i = 0; i < buffer.times.length; i++) {
        xMin = Math.min(xMin, buffer.times[i]); xMax = Math.max(xMax, buffer.times[i]);
        yMin = Math.min(yMin, buffer.values[i]); yMax = Math.max(yMax, buffer.values[i]);
      }
    }
    if (!data.length) { xMin = 0; xMax = 1000; yMin = -1; yMax = 1; }
    const timeOrigin = xMin;
    if (xMax === xMin) { xMin -= 500; xMax += 500; }
    const pad = (yMax - yMin) * .08 || Math.abs(yMin) * .02 || 1;
    yMin -= pad; yMax += pad;
    this.bounds = { xMin, xMax, yMin, yMax };
    const margin = { left: 56, right: 14, top: 14, bottom: 34 };
    const pw = width - margin.left - margin.right, ph = height - margin.top - margin.bottom;
    const tx = (value) => margin.left + (value - xMin) / (xMax - xMin) * pw;
    const ty = (value) => margin.top + (1 - (value - yMin) / (yMax - yMin)) * ph;
    ctx.font = "10px ui-monospace, monospace";
    ctx.lineWidth = 1;
    for (let i = 0; i <= 4; i++) {
      const yValue = yMin + (yMax - yMin) * i / 4;
      const xValue = xMin + (xMax - xMin) * i / 4;
      ctx.strokeStyle = "#edf0f3";
      ctx.beginPath(); ctx.moveTo(margin.left, ty(yValue)); ctx.lineTo(width - margin.right, ty(yValue)); ctx.stroke();
      ctx.beginPath(); ctx.moveTo(tx(xValue), margin.top); ctx.lineTo(tx(xValue), height - margin.bottom); ctx.stroke();
      ctx.fillStyle = "#7b8794";
      ctx.textAlign = "right";
      ctx.fillText(Math.abs(yValue) >= 10000 || (Math.abs(yValue) > 0 && Math.abs(yValue) < .001) ? yValue.toExponential(1) : Number(yValue.toPrecision(4)).toString(), margin.left - 6, ty(yValue) + 3);
      ctx.textAlign = "center";
      ctx.fillText(((xValue - timeOrigin) / 1000).toFixed(2), tx(xValue), height - 19);
    }
    ctx.fillText("时间（s）", margin.left + pw / 2, height - 4);
    ctx.save();
    ctx.beginPath(); ctx.rect(margin.left - 3, margin.top - 3, pw + 6, ph + 6); ctx.clip();
    for (const item of data) {
      ctx.strokeStyle = item.color;
      ctx.fillStyle = item.color;
      ctx.lineWidth = 1.8;
      ctx.lineJoin = "round";
      ctx.beginPath();
      for (let i = 0; i < item.times.length; i++) {
        const x = tx(item.times[i]), y = ty(item.values[i]);
        if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
      }
      ctx.stroke();
      ctx.beginPath(); ctx.arc(tx(item.times.at(-1)), ty(item.values.at(-1)), 3, 0, Math.PI * 2); ctx.fill();
    }
    ctx.restore();
    if (this.series.size && !data.length) {
      ctx.textAlign = "center"; ctx.fillStyle = "#929ba5";
      ctx.fillText("等待字段数据…", width / 2, height / 2);
    }
  }

  destroy() { this.resizeObserver.disconnect(); this.el.remove(); }
}

export function createPlotWindows({ state, onChange, onDrop }) {
  const container = document.getElementById("chartContainer");
  const storageKey = "dzplot.plotWindows";
  let nextId = 1;
  let restored = false;
  const colors = new Map();
  const colorFor = (topic, field) => {
    const key = seriesKey(topic, field);
    if (!colors.has(key)) colors.set(key, seriesColor(topic, field, new Set(colors.values())));
    return colors.get(key);
  };
  const actions = { select, split, close, remove: removeField, drop: (payload, id) => onDrop(payload, id) };
  const leaf = (id) => ({ type: "plot", id });
  const create = (id = `plot_${nextId++}`) => {
    const number = Number(id.slice(5));
    nextId = Math.max(nextId, number + 1);
    state.charts[id] = new PlotWindow(id, number, state, actions);
    return leaf(id);
  };

  function save() {
    try {
      localStorage.setItem(storageKey, JSON.stringify({ version: 1, root: state.plotLayout, active: state.activePlotId,
        plots: Object.values(state.charts).map((plot) => ({ id: plot.id, series: [...plot.series.values()] })) }));
    } catch {}
  }
  function changed() {
    for (const plot of Object.values(state.charts)) {
      plot.el.classList.toggle("active", plot.id === state.activePlotId);
      plot.el.querySelector(".plot-window-close").disabled = Object.keys(state.charts).length === 1;
    }
    const active = state.charts[state.activePlotId];
    document.getElementById("activePlotLabel").textContent = `当前窗口 ${active.number}`;
    document.getElementById("selectedPanelTitle").textContent = `窗口 ${active.number} · 已选曲线`;
    document.getElementById("chartCount").textContent = `${Object.keys(state.charts).length} 个窗口`;
    onChange();
    save();
  }
  function select(id) {
    if (!state.charts[id] || state.activePlotId === id) return;
    state.activePlotId = id;
    changed();
  }
  function selectedFields(id = state.activePlotId) {
    const fields = Object.create(null);
    const plots = id === null ? Object.values(state.charts) : [state.charts[id]];
    for (const plot of plots) for (const { topic, field } of plot?.series.values() || []) {
      (fields[topic] ||= []);
      if (!fields[topic].includes(field)) fields[topic].push(field);
    }
    return fields;
  }
  function addFields(topic, fields, id = state.activePlotId) {
    const plot = state.charts[id];
    if (!plot) return;
    let added = false;
    for (const field of fields) {
      const key = seriesKey(topic, field);
      if (plot.series.has(key)) continue;
      plot.series.set(key, { topic, field, color: colorFor(topic, field) });
      state.chartData[key] ||= { times: [], values: [] };
      added = true;
    }
    if (!added) return;
    plot.updateLegend(); changed();
  }
  function removeField(topic, field, id = state.activePlotId) {
    const plot = state.charts[id];
    if (!plot?.series.delete(seriesKey(topic, field))) return;
    plot.updateLegend(); changed();
  }
  function removeTopic(topic, id = state.activePlotId) {
    const plot = state.charts[id];
    if (!plot) return;
    for (const [key, item] of plot.series) if (item.topic === topic) plot.series.delete(key);
    plot.updateLegend(); changed();
  }
  function clear(id = state.activePlotId) {
    const plot = state.charts[id];
    if (!plot) return;
    plot.series.clear(); plot.updateLegend(); changed();
  }
  function replace(node, id, replacement) {
    if (node.type === "plot") return node.id === id ? replacement : node;
    node.first = replace(node.first, id, replacement);
    node.second = replace(node.second, id, replacement);
    return node;
  }
  function split(direction, id = state.activePlotId) {
    if (!state.charts[id] || !["row", "column"].includes(direction)) return;
    const newLeaf = create();
    state.plotLayout = replace(state.plotLayout, id, { type: "split", direction, ratio: 50, first: leaf(id), second: newLeaf });
    state.activePlotId = newLeaf.id;
    renderLayout(); changed();
  }
  function close(id = state.activePlotId) {
    if (!state.charts[id] || Object.keys(state.charts).length === 1) return;
    let neighbor;
    const firstLeaf = (node) => node.type === "plot" ? node.id : firstLeaf(node.first);
    function collapse(node) {
      if (node.type === "plot") return node.id === id ? null : node;
      node.first = collapse(node.first); node.second = collapse(node.second);
      if (!node.first || !node.second) {
        const remaining = node.first || node.second;
        neighbor = firstLeaf(remaining);
        return remaining;
      }
      return node;
    }
    state.plotLayout = collapse(state.plotLayout);
    state.charts[id].destroy(); delete state.charts[id];
    if (state.activePlotId === id) state.activePlotId = neighbor;
    renderLayout(); changed();
  }
  function merge() {
    const plots = Object.values(state.charts);
    const target = plots[0];
    for (const plot of plots.slice(1)) {
      for (const [key, item] of plot.series) target.series.set(key, item);
      plot.destroy(); delete state.charts[plot.id];
    }
    state.plotLayout = leaf(target.id); state.activePlotId = target.id;
    target.updateLegend(); renderLayout(); changed();
  }
  function renderLayout() {
    function render(node) {
      if (node.type === "plot") {
        const el = state.charts[node.id].el;
        el.style.flex = "1 1 0";
        return el;
      }
      const group = document.createElement("div");
      group.className = `plot-split plot-split-${node.direction}`;
      const first = render(node.first), second = render(node.second);
      const divider = document.createElement("div");
      divider.className = "plot-divider";
      divider.tabIndex = 0;
      divider.setAttribute("role", "separator");
      divider.setAttribute("aria-label", node.direction === "row" ? "调整左右窗口比例" : "调整上下窗口比例");
      divider.setAttribute("aria-orientation", node.direction === "row" ? "vertical" : "horizontal");
      divider.setAttribute("aria-valuemin", "10"); divider.setAttribute("aria-valuemax", "90");
      const resize = (ratio) => {
        node.ratio = Math.min(90, Math.max(10, ratio));
        first.style.flex = `${node.ratio} 1 0`;
        second.style.flex = `${100 - node.ratio} 1 0`;
        divider.setAttribute("aria-valuenow", String(Math.round(node.ratio)));
      };
      let dragging = false;
      divider.onpointerdown = (event) => {
        if (event.button !== 0) return;
        event.preventDefault();
        dragging = true; divider.setPointerCapture(event.pointerId);
      };
      divider.onpointermove = (event) => {
        if (!dragging) return;
        const rect = group.getBoundingClientRect();
        resize(node.direction === "row" ? (event.clientX - rect.left) / rect.width * 100 : (event.clientY - rect.top) / rect.height * 100);
      };
      const finish = () => { dragging = false; save(); };
      divider.onpointerup = finish; divider.onpointercancel = finish; divider.onlostpointercapture = finish;
      divider.onkeydown = (event) => {
        const keys = node.direction === "row" ? ["ArrowLeft", "ArrowRight"] : ["ArrowUp", "ArrowDown"];
        if (!keys.includes(event.key)) return;
        event.preventDefault(); resize(node.ratio + (event.key === keys[0] ? -2 : 2)); save();
      };
      resize(node.ratio);
      group.append(first, divider, second);
      return group;
    }
    container.replaceChildren(render(state.plotLayout));
  }
  function restore() {
    try {
      const config = JSON.parse(localStorage.getItem(storageKey) || "null");
      if (config?.version !== 1 || !Array.isArray(config.plots)) return false;
      const records = new Map(config.plots.map((plot) => [plot.id, plot.series]));
      const ids = new Set();
      function validate(node, depth = 0) {
        if (!node || depth > 12 || ids.size > 64) throw Error("布局无效");
        if (node.type === "plot") {
          if (!/^plot_[1-9]\d{0,5}$/.test(node.id) || ids.has(node.id) || !Array.isArray(records.get(node.id))) throw Error("窗口无效");
          ids.add(node.id); return leaf(node.id);
        }
        if (node.type !== "split" || !["row", "column"].includes(node.direction)) throw Error("分裂方向无效");
        return { type: "split", direction: node.direction, ratio: Number(node.ratio) || 50, first: validate(node.first, depth + 1), second: validate(node.second, depth + 1) };
      }
      state.plotLayout = validate(config.root);
      for (const id of ids) {
        create(id);
        const plot = state.charts[id];
        for (const item of records.get(id)) if (typeof item.topic === "string" && typeof item.field === "string" && item.field) {
          const key = seriesKey(item.topic, item.field);
          if (!colors.has(key) && /^#[a-f\d]{6}$|^hsl\(\d+(?:\.\d+)? 58% 43%\)$/.test(item.color)) colors.set(key, item.color);
          plot.series.set(key, { topic: item.topic, field: item.field, color: colorFor(item.topic, item.field) });
          state.chartData[key] ||= { times: [], values: [] };
        }
        plot.updateLegend();
      }
      state.activePlotId = ids.has(config.active) ? config.active : ids.values().next().value;
      return true;
    } catch {
      for (const plot of Object.values(state.charts)) plot.destroy();
      state.charts = Object.create(null); colors.clear(); nextId = 1;
      return false;
    }
  }
  function init() {
    restored = restore();
    if (!restored) { state.plotLayout = create(); state.activePlotId = state.plotLayout.id; }
    document.getElementById("splitPlotRightBtn").onclick = () => split("row");
    document.getElementById("splitPlotDownBtn").onclick = () => split("column");
    document.getElementById("mergePlotsBtn").onclick = merge;
    document.getElementById("clearChartsBtn").onclick = () => clear();
    renderLayout(); changed();
  }
  return { init, select, split, close, merge, clear, addFields, removeField, removeTopic, selectedFields, get restored() { return restored; } };
}
