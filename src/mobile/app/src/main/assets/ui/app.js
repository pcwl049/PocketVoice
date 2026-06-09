(function () {
  const TEXT = {
    waiting: "\u7b49\u5f85",
    listening: "\u8046\u542c",
    starting: "\u542f\u52a8\u4e2d",
    stopping: "\u505c\u6b62\u4e2d",
    needsCare: "\u9700\u8981\u5904\u7406",
    converting: "\u6b63\u5728\u8f6c\u6362\u8bed\u97f3",
    startTitle: "\u6b63\u5728\u542f\u52a8",
    stopTitle: "\u6b63\u5728\u505c\u6b62",
    errorTitle: "\u9047\u5230\u95ee\u9898",
    ready: "\u51c6\u5907\u597d\u4e86",
    noModel: "\u672a\u627e\u5230\u6a21\u578b\u8def\u5f84",
    waitingFirst: "\u7b49\u5f85\u7b2c\u4e00\u6bb5\u8bed\u97f3\u3002",
    justNow: "\u521a\u521a",
    none: "\u6682\u65e0",
    cache: "\u7f13\u5b58",
    noLogs: "\u6682\u65e0\u65e5\u5fd7",
  };

  const startButton = document.querySelector("[data-start]");
  const stopButton = document.querySelector("[data-stop]");
  const clearLogButton = document.querySelector("[data-clear-log]");
  const statusChip = document.querySelector("[data-status-chip]");
  const statusLabel = document.querySelector("[data-status-label]");
  const serviceTitle = document.querySelector("[data-service-title]");
  const backend = document.querySelector("[data-backend]");
  const modelDir = document.querySelector("[data-model-dir]");
  const port = document.querySelector("[data-port]");
  const lastText = document.querySelector("[data-last-text]");
  const lastAge = document.querySelector("[data-last-age]");
  const recognizeMs = document.querySelector("[data-recognize-ms]");
  const audioMs = document.querySelector("[data-audio-ms]");
  const cacheStats = document.querySelector("[data-cache-stats]");
  const log = document.querySelector("[data-log]");
  const metricCards = Array.from(document.querySelectorAll("[data-metric-card]"));

  let previousText = "";
  let previousRecognize = 0;
  let previousAudio = 0;
  let previousLogKey = "";
  let localStoppingUntil = 0;

  function bridge() {
    return window.STT || null;
  }

  function readSnapshot() {
    const api = bridge();
    if (!api || typeof api.getSnapshot !== "function") {
      return {
        status: "stopped",
        backend: "preview",
        modelDir: "WebView bridge is not connected.",
        port: 27000,
        lastText: "",
        lastAudioMs: 0,
        lastRecognizeMs: 0,
        totalRequests: 0,
        cacheHits: 0,
        logs: ["WebView UI loaded"],
      };
    }

    try {
      return JSON.parse(api.getSnapshot());
    } catch (error) {
      return {
        status: "error",
        backend: "unknown",
        modelDir: "",
        port: 27000,
        lastText: "",
        lastAudioMs: 0,
        lastRecognizeMs: 0,
        totalRequests: 0,
        cacheHits: 0,
        lastError: String(error && error.message ? error.message : error),
        logs: ["Failed to read bridge snapshot"],
      };
    }
  }

  function isStopping() {
    return Date.now() < localStoppingUntil;
  }

  function statusText(status) {
    if (status === "stopping" || isStopping()) return TEXT.stopping;
    if (status === "running") return TEXT.listening;
    if (status === "starting") return TEXT.starting;
    if (status === "error") return TEXT.needsCare;
    return TEXT.waiting;
  }

  function titleText(status) {
    if (status === "stopping" || isStopping()) return TEXT.stopTitle;
    if (status === "running") return TEXT.converting;
    if (status === "starting") return TEXT.startTitle;
    if (status === "error") return TEXT.errorTitle;
    return TEXT.ready;
  }

  function formatDuration(ms) {
    if (!ms) return "--";
    if (ms >= 1000) return `${(ms / 1000).toFixed(2)}s`;
    return `${ms}ms`;
  }

  function pulse(element) {
    element.classList.remove("changed");
    void element.offsetWidth;
    element.classList.add("changed");
  }

  function renderLogs(rows) {
    const visibleRows = (Array.isArray(rows) ? rows : []).slice(-4);
    const nextKey = JSON.stringify(visibleRows);
    if (nextKey === previousLogKey) return;
    previousLogKey = nextKey;

    log.replaceChildren();
    if (visibleRows.length === 0) {
      const empty = document.createElement("div");
      empty.className = "log-row";
      empty.innerHTML = `<span>--:--</span><span>${TEXT.noLogs}</span>`;
      log.appendChild(empty);
      return;
    }

    for (const row of visibleRows) {
      const item = document.createElement("div");
      item.className = "log-row";
      const time = new Date().toLocaleTimeString("zh-CN", { hour: "2-digit", minute: "2-digit" });
      item.innerHTML = `<span>${time}</span><span>${String(row)}</span>`;
      log.appendChild(item);
    }
  }

  function render() {
    const snapshot = readSnapshot();
    const status = snapshot.status || "stopped";
    if (status === "stopped" || status === "error") {
      localStoppingUntil = 0;
    }
    const text = snapshot.lastText || "";
    const currentRecognize = Number(snapshot.lastRecognizeMs || 0);
    const currentAudio = Number(snapshot.lastAudioMs || 0);
    const stopping = status === "stopping" || isStopping();

    statusChip.classList.toggle("running", status === "running" && !stopping);
    statusChip.classList.toggle("starting", status === "starting");
    statusChip.classList.toggle("stopping", stopping);
    statusChip.classList.toggle("error", status === "error");
    statusLabel.textContent = statusText(status);
    serviceTitle.textContent = titleText(status);
    backend.textContent = snapshot.backend || "unknown";
    modelDir.textContent = snapshot.modelDir || TEXT.noModel;
    port.textContent = `tcp:${snapshot.port || 27000}`;
    lastText.textContent = text || TEXT.waitingFirst;
    lastAge.textContent = text ? TEXT.justNow : TEXT.none;
    recognizeMs.textContent = currentRecognize ? `${currentRecognize}ms` : "--";
    audioMs.textContent = formatDuration(currentAudio);
    cacheStats.textContent = `${TEXT.cache} ${snapshot.cacheHits || 0} / ${snapshot.totalRequests || 0}`;

    startButton.disabled = status === "starting" || status === "running" || stopping;
    stopButton.disabled = status === "stopped" && !stopping;
    startButton.classList.toggle("busy", status === "starting");
    stopButton.classList.toggle("active", status === "running" && !stopping);
    stopButton.classList.toggle("stopping", stopping);

    if (text && text !== previousText) pulse(lastText);
    if (currentRecognize !== previousRecognize || currentAudio !== previousAudio) {
      metricCards.forEach(pulse);
    }
    previousText = text;
    previousRecognize = currentRecognize;
    previousAudio = currentAudio;

    renderLogs(snapshot.logs);
  }

  startButton.addEventListener("click", function () {
    const api = bridge();
    startButton.classList.add("pressed");
    setTimeout(() => startButton.classList.remove("pressed"), 180);
    if (api && typeof api.start === "function") api.start();
    render();
  });

  stopButton.addEventListener("click", function () {
    const api = bridge();
    localStoppingUntil = Date.now() + 900;
    stopButton.classList.add("pressed");
    setTimeout(() => stopButton.classList.remove("pressed"), 180);
    if (api && typeof api.stop === "function") api.stop();
    render();
  });

  clearLogButton.addEventListener("click", function () {
    const api = bridge();
    if (api && typeof api.clearLog === "function") api.clearLog();
    previousLogKey = "";
    render();
  });

  render();
  setInterval(render, 500);
}());
