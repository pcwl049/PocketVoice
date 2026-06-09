#include "pc_status_page.h"

namespace stt {

std::string pcStatusPageHtml() {
    return R"HTML(<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>PocketVoice Console</title>
  <link rel="icon" href="data:,">
  <style>
    :root {
      color-scheme: dark;
      --bg: #111312;
      --panel: rgba(29, 32, 30, 0.86);
      --panel-strong: rgba(37, 41, 38, 0.94);
      --line: rgba(229, 224, 211, 0.12);
      --text: #ece8dc;
      --muted: #a6a091;
      --soft: #777366;
      --leaf: #8ebf7b;
      --leaf-dark: #496646;
      --warn: #d8ad6f;
      --bad: #d77568;
      --ok: #8ecf9a;
      --shadow: 0 18px 45px rgba(0, 0, 0, 0.24);
    }

    * { box-sizing: border-box; }

    body {
      margin: 0;
      min-height: 100vh;
      font-family: "Segoe UI", "Microsoft YaHei", Arial, sans-serif;
      color: var(--text);
      background:
        radial-gradient(circle at 15% 12%, rgba(142, 191, 123, 0.10), transparent 30%),
        radial-gradient(circle at 85% 18%, rgba(216, 173, 111, 0.08), transparent 26%),
        linear-gradient(145deg, #101210 0%, #171a17 52%, #101211 100%);
      overflow-x: hidden;
    }

    body::before {
      content: "";
      position: fixed;
      inset: 0;
      pointer-events: none;
      opacity: 0.26;
      background-image:
        linear-gradient(rgba(255,255,255,0.035) 1px, transparent 1px),
        linear-gradient(90deg, rgba(255,255,255,0.025) 1px, transparent 1px);
      background-size: 44px 44px;
      mask-image: linear-gradient(to bottom, transparent, #000 16%, #000 78%, transparent);
    }

    .shell {
      width: min(1180px, calc(100% - 40px));
      margin: 0 auto;
      padding: 30px 0 42px;
    }

    .topbar {
      display: flex;
      align-items: flex-end;
      justify-content: space-between;
      gap: 18px;
      margin-bottom: 22px;
    }

    .brand h1 {
      margin: 0 0 5px;
      font-size: 28px;
      font-weight: 650;
      letter-spacing: 0;
    }

    .brand p {
      margin: 0;
      color: var(--muted);
      font-size: 14px;
    }

    .poll {
      display: flex;
      align-items: center;
      gap: 10px;
      color: var(--muted);
      font-size: 13px;
      white-space: nowrap;
    }

    .top-actions {
      display: flex;
      align-items: center;
      flex-wrap: wrap;
      gap: 8px;
      margin-top: 10px;
    }

    .dot {
      width: 9px;
      height: 9px;
      border-radius: 50%;
      background: var(--soft);
      box-shadow: 0 0 0 4px rgba(255,255,255,0.04);
    }

    .dot.ok { background: var(--ok); }
    .dot.warn { background: var(--warn); }
    .dot.bad { background: var(--bad); }

    .grid {
      display: grid;
      grid-template-columns: 1.15fr 0.85fr;
      gap: 18px;
    }

    .panel {
      background: var(--panel);
      border: 1px solid var(--line);
      border-radius: 8px;
      box-shadow: var(--shadow);
      backdrop-filter: blur(16px);
    }

    .panel.pad { padding: 18px; }

    .status-grid {
      display: grid;
      grid-template-columns: repeat(4, minmax(0, 1fr));
      gap: 10px;
      margin-bottom: 18px;
    }

    .status {
      min-height: 76px;
      padding: 14px;
      background: rgba(255,255,255,0.035);
      border: 1px solid rgba(255,255,255,0.07);
      border-radius: 8px;
    }

    .status span {
      display: block;
      color: var(--muted);
      font-size: 12px;
      margin-bottom: 8px;
    }

    .status strong {
      display: flex;
      align-items: center;
      gap: 8px;
      font-size: 17px;
      font-weight: 620;
    }

    .text-card {
      min-height: 260px;
      display: flex;
      flex-direction: column;
      background: var(--panel-strong);
      border-radius: 8px;
      border: 1px solid var(--line);
      overflow: hidden;
    }

    .section-title {
      display: flex;
      align-items: center;
      justify-content: space-between;
      padding: 14px 16px;
      border-bottom: 1px solid var(--line);
      color: var(--muted);
      font-size: 13px;
    }

    .actions {
      display: flex;
      flex-wrap: wrap;
      gap: 8px;
      margin: 0 0 14px;
    }

    button {
      height: 34px;
      padding: 0 12px;
      border: 1px solid rgba(255,255,255,0.10);
      border-radius: 7px;
      color: var(--text);
      background: rgba(255,255,255,0.055);
      font: inherit;
      font-size: 13px;
      cursor: pointer;
      transition: background 150ms ease, border-color 150ms ease, transform 150ms ease;
    }

    button:hover {
      background: rgba(142, 191, 123, 0.13);
      border-color: rgba(142, 191, 123, 0.32);
    }

    button:active {
      transform: translateY(1px);
    }

    button.warn:hover {
      background: rgba(216, 173, 111, 0.13);
      border-color: rgba(216, 173, 111, 0.34);
    }

    select {
      width: 100%;
      min-height: 38px;
      padding: 0 12px;
      border: 1px solid rgba(255,255,255,0.10);
      border-radius: 7px;
      color: var(--text);
      background: rgba(255,255,255,0.055);
      font: inherit;
      font-size: 13px;
      outline: none;
      transition: background 150ms ease, border-color 150ms ease;
    }

    select:hover,
    select:focus {
      background: rgba(142, 191, 123, 0.10);
      border-color: rgba(142, 191, 123, 0.30);
    }

    option {
      color: #111312;
      background: #ece8dc;
    }

    .field {
      display: grid;
      gap: 8px;
      margin-bottom: 12px;
    }

    .field label {
      color: var(--muted);
      font-size: 12px;
    }

    .device-note {
      min-height: 18px;
      color: var(--muted);
      font-size: 12px;
      line-height: 1.35;
      word-break: break-word;
    }

    .feedback {
      min-height: 22px;
      margin-bottom: 10px;
      color: var(--muted);
      font-size: 13px;
    }

    .latest-text {
      flex: 1;
      padding: 22px;
      font-size: 24px;
      line-height: 1.55;
      word-break: break-word;
      color: #f4efe1;
    }

    .placeholder {
      color: #767165;
    }

    .side {
      display: grid;
      gap: 18px;
    }

    .metrics {
      display: grid;
      grid-template-columns: repeat(2, minmax(0, 1fr));
      gap: 10px;
    }

    .metric {
      padding: 14px;
      border-radius: 8px;
      background: rgba(255,255,255,0.035);
      border: 1px solid rgba(255,255,255,0.07);
    }

    .metric span {
      display: block;
      margin-bottom: 8px;
      color: var(--muted);
      font-size: 12px;
    }

    .metric strong {
      font-size: 24px;
      font-weight: 650;
    }

    .queue-text {
      margin-top: 12px;
      color: var(--muted);
      font-size: 13px;
      line-height: 1.45;
      word-break: break-word;
    }

    .error {
      min-height: 70px;
      padding: 14px;
      color: #e8d3c8;
      background: rgba(215, 117, 104, 0.08);
      border: 1px solid rgba(215, 117, 104, 0.18);
      border-radius: 8px;
      line-height: 1.45;
      word-break: break-word;
    }

    .muted { color: var(--muted); }
    .ok-text { color: var(--ok); }
    .warn-text { color: var(--warn); }
    .bad-text { color: var(--bad); }

    @media (max-width: 860px) {
      .shell { width: min(100% - 24px, 1180px); padding-top: 22px; }
      .topbar { align-items: flex-start; flex-direction: column; }
      .grid { grid-template-columns: 1fr; }
      .status-grid { grid-template-columns: repeat(2, minmax(0, 1fr)); }
      .latest-text { font-size: 20px; }
    }

    @media (max-width: 520px) {
      .status-grid, .metrics { grid-template-columns: 1fr; }
      .brand h1 { font-size: 24px; }
    }
  </style>
</head>
)HTML" R"HTML(<body>
  <main class="shell">
    <header class="topbar">
      <div class="brand">
        <h1>PocketVoice Console</h1>
        <div class="top-actions">
          <button id="listen-btn" type="button">停止监听</button>
          <button id="reconnect-btn" type="button">重连手机</button>
        </div>
        <p>PC 端只读运行面板</p>
      </div>
      <div class="poll"><span id="poll-dot" class="dot warn"></span><span id="poll-text">连接状态读取中</span></div>
    </header>

    <section class="grid">
      <div>
        <div class="status-grid">
          <div class="status"><span>手机连接</span><strong><i id="phone-dot" class="dot"></i><b id="phone">--</b></strong></div>
          <div class="status"><span>OSC</span><strong><i id="osc-dot" class="dot"></i><b id="osc">--</b></strong></div>
          <div class="status"><span>ChatBox</span><strong><i id="queue-dot" class="dot"></i><b id="queue-state">--</b></strong></div>
          <div class="status"><span>Typing</span><strong><i id="typing-dot" class="dot"></i><b id="typing">--</b></strong></div>
        </div>

        <div class="text-card">
          <div class="section-title">
            <span>最近识别文本</span>
            <span id="emotion" class="muted">NEUTRAL</span>
          </div>
          <div id="latest-text" class="latest-text placeholder">等待文本</div>
        </div>
      </div>

      <aside class="side">
        <section class="panel pad">
          <div class="section-title" style="padding:0 0 12px;border-bottom:0">
            <span>ChatBox 队列</span>
            <span id="dry-run" class="muted">dry-run: --</span>
          </div>
          <div class="actions">
            <button id="pause-btn" type="button">暂停发送</button>
            <button id="clear-queue-btn" class="warn" type="button">清空队列</button>
            <button id="clear-chatbox-btn" class="warn" type="button">清空 ChatBox</button>
          </div>
          <div id="feedback" class="feedback">等待操作</div>
          <div class="metrics">
            <div class="metric"><span>等待</span><strong id="pending">0</strong></div>
            <div class="metric"><span>已发送</span><strong id="sent">0</strong></div>
            <div class="metric"><span>失败</span><strong id="failed">0</strong></div>
            <div class="metric"><span>重复跳过</span><strong id="skipped">0</strong></div>
          </div>
          <div class="queue-text">上次发送：<span id="last-sent">无</span></div>
        </section>

        <section class="panel pad">
          <div class="section-title" style="padding:0 0 12px;border-bottom:0">
            <span>运行信息</span>
            <span id="running" class="muted">--</span>
          </div>
          <div class="field">
            <label for="audio-device-select">音频输入</label>
            <select id="audio-device-select">
              <option value="">默认录音设备</option>
            </select>
            <div id="audio-device-note" class="device-note">等待设备列表</div>
          </div>
          <div class="metrics">
            <div class="metric"><span>音频段</span><strong id="audio-count">0</strong></div>
            <div class="metric"><span>刷新</span><strong id="refresh-count">0</strong></div>
          </div>
        </section>

        <section class="panel pad">
          <div class="section-title" style="padding:0 0 12px;border-bottom:0">
            <span>最近错误</span>
            <button id="clear-error-btn" type="button">清空错误</button>
          </div>
          <div id="error" class="error muted">无</div>
        </section>
      </aside>
    </section>
  </main>

  <script>
    const $ = (id) => document.getElementById(id);
    let refreshCount = 0;
    let queuePaused = false;
    let listeningActive = true;
    let selectedAudioDeviceId = "";
    let deviceSelectDirty = false;

    function setBool(id, dotId, value, trueText, falseText) {
      $(id).textContent = value ? trueText : falseText;
      const dot = $(dotId);
      dot.className = "dot " + (value ? "ok" : "bad");
    }

    function text(value, fallback = "无") {
      return value && String(value).trim() ? value : fallback;
    }

    function renderAudioDevices(audioInput) {
      const select = $("audio-device-select");
      const note = $("audio-device-note");
      const input = audioInput || {};
      const devices = Array.isArray(input.devices) ? input.devices : [];
      const currentId = input.selected_device_id || "";
      selectedAudioDeviceId = currentId;

      if (!deviceSelectDirty) {
        const options = ['<option value="">默认录音设备</option>'].concat(devices.map((device) => {
          const label = (device.name || device.id || "音频输入") + (device.is_default ? " · 默认" : "");
          return `<option value="${escapeHtml(device.id || "")}">${escapeHtml(label)}</option>`;
        }));
        select.innerHTML = options.join("");
        select.value = currentId;
      }

      const loopback = input.mode === "loopback";
      select.disabled = loopback || devices.length === 0;
      if (loopback) {
        note.textContent = "当前使用播放回环";
      } else if (devices.length === 0) {
        note.textContent = "没有检测到可用录音设备";
      } else {
        note.textContent = text(input.selected_device_name, "默认录音设备");
      }
    }

    function escapeHtml(value) {
      return String(value)
        .replaceAll("&", "&amp;")
        .replaceAll("<", "&lt;")
        .replaceAll(">", "&gt;")
        .replaceAll('"', "&quot;");
    }

    async function refresh() {
      try {
        const response = await fetch("/status", { cache: "no-store" });
        if (!response.ok) throw new Error("HTTP " + response.status);
        const data = await response.json();
        refreshCount += 1;
        listeningActive = !!data.listening_active;
        $("listen-btn").textContent = listeningActive ? "停止监听" : "开始监听";
        $("reconnect-btn").textContent = data.reconnecting ? "重连中" : "重连手机";
        $("reconnect-btn").disabled = !!data.reconnecting;

        $("poll-dot").className = "dot ok";
        $("poll-text").textContent = "状态已同步";
        setBool("phone", "phone-dot", !!data.phone_connected, "已连接", "未连接");
        setBool("osc", "osc-dot", !!data.osc_ready, "可用", "不可用");
        setBool("typing", "typing-dot", !!data.typing_active, "输入中", "等待");

        const queue = data.chatbox || {};
        const sending = !!queue.sending;
        queuePaused = !!queue.paused;
        $("queue-state").textContent = queuePaused ? "已暂停" : (sending ? "发送中" : "空闲");
        $("queue-dot").className = "dot " + (queuePaused ? "warn" : (sending ? "warn" : "ok"));
        $("pause-btn").textContent = queuePaused ? "恢复发送" : "暂停发送";
        $("pending").textContent = queue.pending_count ?? 0;
        $("sent").textContent = queue.sent_count ?? 0;
        $("failed").textContent = queue.failed_count ?? 0;
        $("skipped").textContent = queue.skipped_duplicate_count ?? 0;
        $("last-sent").textContent = text(queue.last_sent_text);

        $("dry-run").textContent = "dry-run: " + (data.chatbox_dry_run ? "on" : "off");
        $("running").textContent = data.running ? "运行中" : "已停止";
        $("audio-count").textContent = data.sent_audio_count ?? 0;
        $("refresh-count").textContent = refreshCount;
        renderAudioDevices(data.audio_input);
        $("emotion").textContent = text(data.last_emotion, "NEUTRAL");

        const latest = $("latest-text");
        latest.textContent = text(data.last_text, "等待文本");
        latest.classList.toggle("placeholder", !data.last_text);

        const err = data.last_error || queue.last_error || "";
        $("error").textContent = text(err);
        $("error").classList.toggle("muted", !err);
      } catch (err) {
        $("poll-dot").className = "dot bad";
        $("poll-text").textContent = "状态读取失败";
        $("error").textContent = err.message || String(err);
        $("error").classList.remove("muted");
      }
    }

    async function postControl(path, label, payload) {
      $("feedback").textContent = label + "...";
      try {
        const request = { method: "POST", cache: "no-store" };
        if (payload) {
          request.headers = { "Content-Type": "application/json" };
          request.body = JSON.stringify(payload);
        }
        const response = await fetch(path, request);
        const data = await response.json();
        $("feedback").textContent = data.ok ? data.message : (data.message || "操作失败");
        await refresh();
      } catch (err) {
        $("feedback").textContent = err.message || String(err);
      }
    }

    $("pause-btn").addEventListener("click", () => {
      postControl(queuePaused ? "/control/queue/resume" : "/control/queue/pause", queuePaused ? "恢复发送" : "暂停发送");
    });
    $("listen-btn").addEventListener("click", () => {
      postControl(listeningActive ? "/control/listen/stop" : "/control/listen/start", listeningActive ? "停止监听" : "开始监听");
    });
    $("reconnect-btn").addEventListener("click", () => postControl("/control/phone/reconnect", "重连手机"));
    $("audio-device-select").addEventListener("focus", () => { deviceSelectDirty = true; });
    $("audio-device-select").addEventListener("change", async () => {
      const nextId = $("audio-device-select").value;
      deviceSelectDirty = false;
      await postControl("/control/audio/input-device", "切换音频输入", { id: nextId });
      selectedAudioDeviceId = nextId;
    });
    $("audio-device-select").addEventListener("blur", () => { deviceSelectDirty = false; });
    $("clear-queue-btn").addEventListener("click", () => postControl("/control/queue/clear", "清空队列"));
    $("clear-chatbox-btn").addEventListener("click", () => postControl("/control/chatbox/clear", "清空 ChatBox"));
    $("clear-error-btn").addEventListener("click", () => postControl("/control/error/clear", "清空错误"));

    refresh();
    setInterval(refresh, 700);
  </script>
</body>
</html>)HTML";
}

}
