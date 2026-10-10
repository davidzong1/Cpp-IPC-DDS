/** 工作区布局、子页面通信与持久化。 */
export function createWorkspace({ state, plots, removeField, notify }) {
  const $ = (id) => document.getElementById(id);
  const shell = $("appShell");
  const frame = $("vizFrame");
  let ready = false;
  let pending = [];
  let view = "plot";
  let firstSampleMs = null;
  let sourceFocus = null;
  const preferences = { sidebar: 320, ratio: 55, sidebarHidden: innerWidth <= 620 };
  try { Object.assign(preferences, JSON.parse(localStorage.getItem("dzplot.layout") || "{}")); } catch {}
  const save = () => {
    try { localStorage.setItem("dzplot.layout", JSON.stringify(preferences)); } catch {}
  };
  const redraw = () => Object.values(state.charts).forEach((chart) => { chart.markDirty(); chart._draw(); });
  function applyLayout() {
    shell.style.setProperty("--sidebar-w", `${Math.max(200, Math.min(600, Number(preferences.sidebar) || 320))}px`);
    shell.style.setProperty("--split-ratio", `${Math.max(25, Math.min(75, Number(preferences.ratio) || 55))}%`);
    shell.classList.toggle("sidebar-hidden", preferences.sidebarHidden);
    $("toggleSidebarBtn").classList.toggle("active", !preferences.sidebarHidden);
    $("toggleSidebarBtn").setAttribute("aria-pressed", String(!preferences.sidebarHidden));
    redraw();
  }
  function post(action, payload = {}) {
    const message = { channel: "dzplot.workspace", action, ...payload };
    if (ready) frame.contentWindow.postMessage(message, location.origin);
    else pending.push(message);
  }
  function setView(next, updateHash = true) {
    view = ["plot", "viz", "split"].includes(next) ? next : "plot";
    shell.dataset.view = view;
    $("plotPane").hidden = view === "viz";
    $("vizPane").hidden = view === "plot";
    $("paneDivider").hidden = view !== "split";
    document.querySelectorAll(".workspace-tab").forEach((tab) => {
      const selected = tab.dataset.view === view;
      tab.classList.toggle("active", selected);
      tab.setAttribute("aria-selected", String(selected));
      tab.tabIndex = selected ? 0 : -1;
    });
    $("workspaceHint").textContent = { plot: "曲线工作区", viz: "3D 子页面", split: "曲线与 3D 共用数据源" }[view];
    if (view !== "plot" && !frame.getAttribute("src")) frame.src = "/viz/?embedded=1";
    if (frame.getAttribute("src")) post("layout", { view, visible: view !== "plot" });
    for (const id of ["resetCameraBtn", "toggleGridBtn", "toggleDisplaysBtn"]) $(id).disabled = view === "plot";
    $("railSplitBtn").classList.toggle("active", view === "split");
    if (updateHash) history.replaceState(null, "", `#${view}`);
    redraw();
  }
  function openSource(mode) {
    sourceFocus = document.activeElement;
    $("sourceModal").hidden = false;
    if (mode === "bag") $("tabBag").click();
    if (mode === "live") $("tabLive").click();
    (state.sourceMode === "bag" ? $("bagPath") : $("liveTopic")).focus();
  }
  function closeSource() { $("sourceModal").hidden = true; sourceFocus?.focus(); }
  function prepareDisplay(topic = "", msgType = "") {
    if (view === "plot") setView("split");
    post("prepare_display", { topic, msgType, source: state.topics[topic]?.source });
  }
  document.querySelectorAll(".workspace-tab").forEach((tab) => {
    tab.onclick = () => setView(tab.dataset.view);
    tab.onkeydown = (event) => {
      const tabs = [...document.querySelectorAll(".workspace-tab")];
      const index = tabs.indexOf(tab);
      if (["ArrowLeft", "ArrowRight", "Home", "End"].includes(event.key)) {
        event.preventDefault();
        const next = event.key === "Home" ? 0 : event.key === "End" ? 2 : (index + (event.key === "ArrowRight" ? 1 : 2)) % 3;
        tabs[next].click(); tabs[next].focus();
      }
    };
  });
  $("openSourceBtn").onclick = () => openSource();
  $("quickBagBtn").onclick = () => openSource("bag");
  $("quickLiveBtn").onclick = () => openSource("live");
  $("closeSourceBtn").onclick = closeSource;
  $("sourceModal").onclick = (event) => { if (event.target === $("sourceModal")) closeSource(); };
  window.addEventListener("keydown", (event) => {
    if (event.key === "Escape") closeSource();
    if (event.key === "Tab" && !$("sourceModal").hidden) {
      const nodes = [...$("sourceModal").querySelectorAll("button,input,select")].filter((node) => node.getClientRects().length);
      if (event.shiftKey && document.activeElement === nodes[0]) { event.preventDefault(); nodes.at(-1).focus(); }
      else if (!event.shiftKey && document.activeElement === nodes.at(-1)) { event.preventDefault(); nodes[0].focus(); }
    }
  });
  $("addDisplayBtn").onclick = () => prepareDisplay();
  $("railSplitBtn").onclick = () => setView(view === "split" ? "plot" : "split");
  $("resetCameraBtn").onclick = () => post("reset_camera");
  $("toggleGridBtn").onclick = () => post("toggle_grid");
  $("toggleDisplaysBtn").onclick = () => post("toggle_displays");
  const toggleSidebar = () => { preferences.sidebarHidden = !preferences.sidebarHidden; save(); applyLayout(); };
  $("toggleSidebarBtn").onclick = () => view === "viz" ? post("toggle_displays") : toggleSidebar();
  $("toggleSidebarTopBtn").onclick = toggleSidebar;
  $("fullscreenBtn").onclick = async () => {
    try { if (document.fullscreenElement) await document.exitFullscreen(); else await shell.requestFullscreen(); }
    catch { notify("浏览器未允许全屏，请使用浏览器的全屏快捷键。"); }
  };
  $("resetLayoutBtn").onclick = () => {
    Object.assign(preferences, { sidebar: 320, ratio: 55, sidebarHidden: innerWidth <= 620 });
    plots.merge();
    save(); applyLayout(); setView("plot");
  };
  function bindDivider(handle, kind) {
    let dragging = false;
    let start = 0;
    let size = 0;
    const vertical = () => kind === "pane" && matchMedia("(max-width: 900px)").matches;
    handle.onpointerdown = (event) => {
      if (event.button !== 0) return;
      dragging = true;
      start = vertical() ? event.clientY : event.clientX;
      size = kind === "sidebar" ? $("dataSidebar").getBoundingClientRect().width : preferences.ratio;
      handle.setPointerCapture(event.pointerId);
      document.body.classList.add("resizing");
      event.preventDefault();
    };
    handle.onpointermove = (event) => {
      if (!dragging) return;
      const delta = (vertical() ? event.clientY : event.clientX) - start;
      if (kind === "sidebar") preferences.sidebar = Math.max(200, Math.min(600, innerWidth * .6, size + delta));
      else {
        const total = vertical() ? $("workbench").clientHeight : $("workbench").clientWidth;
        preferences.ratio = Math.max(25, Math.min(75, size + delta / Math.max(1, total) * 100));
      }
      applyLayout();
    };
    const finish = () => { dragging = false; document.body.classList.remove("resizing"); save(); };
    handle.onpointerup = finish; handle.onpointercancel = finish; handle.onlostpointercapture = finish;
    handle.onkeydown = (event) => {
      if (!["ArrowLeft", "ArrowRight", "ArrowUp", "ArrowDown"].includes(event.key)) return;
      event.preventDefault();
      const direction = ["ArrowRight", "ArrowDown"].includes(event.key) ? 1 : -1;
      if (kind === "sidebar") preferences.sidebar = Math.max(200, Math.min(600, preferences.sidebar + direction * 10));
      else preferences.ratio = Math.max(25, Math.min(75, preferences.ratio + direction * 2));
      save(); applyLayout();
    };
  }
  bindDivider($("sidebarDivider"), "sidebar"); bindDivider($("paneDivider"), "pane");
  window.addEventListener("message", (event) => {
    if (event.origin !== location.origin || event.source !== frame.contentWindow || event.data?.channel !== "dzplot.workspace") return;
    if (event.data.action === "ready") {
      ready = true;
      frame.contentWindow.postMessage({ channel: "dzplot.workspace", action: "layout", view, visible: view !== "plot" }, location.origin);
      for (const message of pending) frame.contentWindow.postMessage(message, location.origin);
      pending = [];
    }
    if (event.data.action === "grid_state") {
      $("toggleGridBtn").classList.toggle("active", event.data.visible);
      $("toggleGridBtn").setAttribute("aria-pressed", String(event.data.visible));
    }
    if (event.data.action === "plot_topic") { setView("split"); preferences.sidebarHidden = false; applyLayout(); $("topicFilter").value = event.data.topic || ""; filter(); }
    if (event.data.action === "error") notify(event.data.message);
  });
  function filter() {
    const query = $("topicFilter").value.trim().toLowerCase();
    for (const item of $("topicList").children) {
      const topicMatch = item.dataset.topic.toLowerCase().includes(query) || (state.topics[item.dataset.topic]?.msg_type || "").toLowerCase().includes(query);
      let fieldMatch = false;
      for (const row of item.querySelectorAll(".field-row")) {
        row.hidden = !topicMatch && !row.dataset.field.toLowerCase().includes(query);
        fieldMatch ||= !row.hidden;
      }
      item.hidden = Boolean(query) && !topicMatch && !fieldMatch;
      item.querySelector(".topic-item-fields").hidden = Boolean(state.collapsedTopics[item.dataset.topic]) && !query;
    }
  }
  $("topicFilter").oninput = filter;
  $("expandTopicsBtn").onclick = () => {
    const collapse = Object.keys(state.topics).some((topic) => !state.collapsedTopics[topic]);
    for (const topic of Object.keys(state.topics)) state.collapsedTopics[topic] = collapse;
    for (const item of $("topicList").children) item.querySelector(".topic-expand").textContent = collapse ? "▸" : "▾";
    $("expandTopicsBtn").textContent = collapse ? "⌄" : "⌃"; filter();
  };
  function updateSeries() {
    const list = $("seriesList"); list.replaceChildren();
    const entries = Object.entries(plots.selectedFields()).flatMap(([topic, fields]) => fields.map((field) => ({ topic, field })));
    $("seriesCount").textContent = entries.length;
    if (!entries.length) { const hint = document.createElement("span"); hint.className = "empty-hint"; hint.textContent = "尚未选择字段"; list.append(hint); }
    for (const { topic, field } of entries) {
      const row = document.createElement("div"); row.className = "series-item";
      const color = document.createElement("i"); color.className = "series-color"; color.style.background = state.charts[state.activePlotId]?.series.get(`${topic}\x00${field}`)?.color || "#347aba";
      const title = document.createElement("span"); title.textContent = `${topic} / ${field}`; title.title = title.textContent;
      const close = document.createElement("button"); close.className = "icon-button"; close.textContent = "×"; close.setAttribute("aria-label", `移除 ${title.textContent}`); close.onclick = () => removeField(topic, field);
      row.append(color, title, close); list.append(row);
    }
  }
  function sampleTime(ms) {
    firstSampleMs ??= ms;
    $("latestTime").textContent = `${Math.max(0, (ms - firstSampleMs) / 1000).toFixed(3)} s`;
  }
  function updatePlayback() {
    const playback = state.playback;
    const hasBag = playback.source === "bag";
    $("btnPause").disabled = !hasBag || playback.mode !== "playing";
    $("btnPlay").disabled = !hasBag || !["paused", "finished", "idle"].includes(playback.mode);
    $("btnStop").disabled = playback.mode === "idle";
    if (playback.progress !== undefined) $("playbackProgress").value = playback.progress;
    if (hasBag && playback.elapsed_s !== undefined) $("latestTime").textContent = `${playback.elapsed_s.toFixed(3)} s`;
    if (hasBag && document.activeElement !== $("bagSpeed")) $("bagSpeed").value = playback.speed || 1;
    if (hasBag) $("bagLoop").checked = playback.loop === true;
    $("sourceSummary").textContent = playback.path || (playback.source === "live" ? "实时话题 · 正在嗅探" : Object.keys(state.topics).length ? "工作区与 3D 共用数据" : "尚未加载数据源");
    $("sourceSummary").title = $("sourceSummary").textContent;
  }
  window.addEventListener("hashchange", () => setView(location.hash.slice(1), false));
  applyLayout(); setView(location.hash.slice(1) || "plot", false);
  return { setView, prepareDisplay, filter, updateSeries, updatePlayback, sampleTime, closeSource, resetTime: () => { firstSampleMs = null; $("playbackProgress").value = 0; }, get view() { return view; } };
}
