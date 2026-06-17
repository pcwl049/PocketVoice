(function () {
  const TEXT = {
    waiting: "等待",
    listening: "监听中",
    starting: "启动中",
    stopping: "停止中",
    needsCare: "需要处理",
    converting: "正在转换语音",
    startTitle: "正在启动",
    stopTitle: "正在停止",
    errorTitle: "遇到问题",
    ready: "准备好了",
    noModel: "未找到模型路径",
    waitingFirst: "等待第一段语音。",
    justNow: "刚刚",
    none: "暂无",
    cache: "缓存",
    noLogs: "暂无日志",
    importIdle: "模型包：未导入"
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
  const policySummary = document.querySelector("[data-policy-summary]");
  const importStatus = document.querySelector("[data-import-status]");
  const cacheToggle = document.querySelector("[data-cache-toggle]");
  const log = document.querySelector("[data-log]");
  const metricCards = Array.from(document.querySelectorAll("[data-metric-card]"));
  const openSettingsButton = document.querySelector("[data-open-settings]");
  const closeSettingsButtons = Array.from(document.querySelectorAll("[data-close-settings]"));
  const importModelPackButton = document.querySelector("[data-import-model-pack]");
  const importNote = document.querySelector("[data-import-note]");
  const sheetShell = document.querySelector("[data-sheet-shell]");
  const sheetBackend = document.querySelector("[data-sheet-backend]");
  const sheetNote = document.querySelector("[data-sheet-note]");
  const policyButtons = Array.from(document.querySelectorAll("[data-policy-option]"));

  let previousText = "";
  let previousRecognize = 0;
  let previousAudio = 0;
  let previousLogKey = "";
  let localStoppingUntil = 0;
  let isSheetOpen = false;

  function bridge() {
    return window.STT || null;
  }

  function readSnapshot() {
    const api = bridge();
    if (!api || typeof api.getSnapshot !== "function") {
      return {
        status: "stopped",
        backend: "preview",
        backendPolicy: "auto",
        modelDir: "WebView bridge is not connected.",
        importStatus: "",
        port: 27000,
        lastText: "",
        lastAudioMs: 0,
        lastRecognizeMs: 0,
        totalRequests: 0,
        cacheHits: 0,
        logs: ["WebView UI loaded"]
      };
    }

    try {
      return JSON.parse(api.getSnapshot());
    } catch (error) {
      return {
        status: "error",
        backend: "unknown",
        backendPolicy: "auto",
        modelDir: "",
        importStatus: "",
        port: 27000,
        lastText: "",
        lastAudioMs: 0,
        lastRecognizeMs: 0,
        totalRequests: 0,
        cacheHits: 0,
        lastError: String(error && error.message ? error.message : error),
        logs: ["Failed to read bridge snapshot"]
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

  function formatBackend(snapshot) {
    const value = snapshot.backend || "unknown";
    return snapshot.cpuFallback ? `${value} CPU fallback` : value;
  }

  function policyText(policy) {
    if (policy === "standard") return "标准中文";
    if (policy === "fast") return "极速";
    if (policy === "mixed") return "兼顾中英";
    if (policy === "rescue") return "兼容兜底";
    return "自动";
  }

  function policyNote(policy) {
    if (policy === "standard") return "优先使用 SenseVoice QNN。";
    if (policy === "fast") return "优先使用低延迟后端。";
    if (policy === "mixed") return "优先使用中英混合场景表现更稳的后端。";
    if (policy === "rescue") return "优先使用兼容性更高的兜底路径。";
    return "系统会按当前策略自动选择实际后端。";
  }

  function pulse(element) {
    element.classList.remove("changed");
    void element.offsetWidth;
    element.classList.add("changed");
  }

  function setSheetOpen(nextOpen) {
    isSheetOpen = nextOpen;
    if (!sheetShell) return;
    if (nextOpen) {
      sheetShell.hidden = false;
      requestAnimationFrame(() => sheetShell.classList.add("open"));
    } else {
      sheetShell.classList.remove("open");
      setTimeout(() => {
        if (!isSheetOpen) {
          sheetShell.hidden = true;
        }
      }, 220);
    }
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
    const policy = snapshot.backendPolicy || "auto";
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
    backend.textContent = formatBackend(snapshot);
    backend.classList.toggle("cpu-fallback", !!snapshot.cpuFallback);
    modelDir.textContent = snapshot.modelDir || TEXT.noModel;
    port.textContent = `tcp:${snapshot.port || 27000}`;
    lastText.textContent = text || TEXT.waitingFirst;
    lastAge.textContent = text ? TEXT.justNow : TEXT.none;
    recognizeMs.textContent = currentRecognize ? `${currentRecognize}ms` : "--";
    audioMs.textContent = formatDuration(currentAudio);
    cacheStats.textContent = `${TEXT.cache} ${snapshot.cacheHits || 0} / ${snapshot.totalRequests || 0}`;
    policySummary.textContent = `策略：${policyText(policy)}`;
    importStatus.textContent = snapshot.importStatus || TEXT.importIdle;
    cacheToggle.checked = !!snapshot.cacheEnabled;

    if (sheetBackend) {
      sheetBackend.textContent = formatBackend(snapshot);
    }
    if (sheetNote) {
      sheetNote.textContent = policyNote(policy);
    }
    if (importNote) {
      importNote.textContent = snapshot.importStatus || "支持 zip 文件，导入后自动放入 models 目录。";
    }
    policyButtons.forEach((button) => {
      button.classList.toggle("selected", button.dataset.policy === policy);
    });

    startButton.disabled = status === "starting" || status === "running" || stopping;
    stopButton.disabled = status === "stopped" && !stopping;
    startButton.classList.toggle("busy", status === "starting");
    stopButton.classList.toggle("active", status === "running" && !stopping);
    stopButton.classList.toggle("stopping", stopping);

    if (text && text !== previousText) {
      pulse(lastText);
    }
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
    if (api && typeof api.start === "function") {
      api.start();
    }
    render();
  });

  stopButton.addEventListener("click", function () {
    const api = bridge();
    localStoppingUntil = Date.now() + 900;
    stopButton.classList.add("pressed");
    setTimeout(() => stopButton.classList.remove("pressed"), 180);
    if (api && typeof api.stop === "function") {
      api.stop();
    }
    render();
  });

  clearLogButton.addEventListener("click", function () {
    const api = bridge();
    if (api && typeof api.clearLog === "function") {
      api.clearLog();
    }
    previousLogKey = "";
    render();
  });

  cacheToggle.addEventListener("change", function () {
    const api = bridge();
    if (api && typeof api.setCacheEnabled === "function") {
      api.setCacheEnabled(!!cacheToggle.checked);
    }
    render();
  });

  if (openSettingsButton) {
    openSettingsButton.addEventListener("click", function () {
      setSheetOpen(true);
    });
  }

  closeSettingsButtons.forEach((button) => {
    button.addEventListener("click", function () {
      setSheetOpen(false);
    });
  });

  if (importModelPackButton) {
    importModelPackButton.addEventListener("click", function () {
      const api = bridge();
      if (api && typeof api.importModelPack === "function") {
        api.importModelPack();
      }
    });
  }

  policyButtons.forEach((button) => {
    button.addEventListener("click", function () {
      const api = bridge();
      const policy = button.dataset.policy;
      if (api && typeof api.setBackendPolicy === "function") {
        api.setBackendPolicy(policy);
      }
      render();
    });
  });

  render();
  setInterval(render, 500);
}());
