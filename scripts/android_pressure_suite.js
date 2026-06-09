#!/usr/bin/env node

const fs = require("fs");
const net = require("net");
const path = require("path");
const { spawnSync } = require("child_process");
const {
  characterErrorRate,
  parseExpectedTsv,
  textMatchesExpectation,
} = require("./pc_wav_suite");

const DEFAULT_ROOT = path.resolve(__dirname, "..");
const DEFAULT_ADB = "D:\\Android\\Sdk\\platform-tools\\adb.exe";
const PACKAGE = "com.stt.mobile";
const APP = "com.stt.mobile/.MainActivity";
const DEFAULT_PORT = 27000;
const MAGIC = 0x5354;
const MESSAGE_TYPE_AUDIO = 0x01;
const MESSAGE_TYPE_TEXT = 0x02;
const MESSAGE_TYPE_ERROR = 0x04;
const FLAG_HAS_SEGMENT_ID = 0x04;

function usage(exitCode) {
  const script = path.basename(process.argv[1]);
  console.log(`Usage: node scripts/${script} [expected.tsv] [options]`);
  console.log("");
  console.log("Options:");
  console.log("  --count N                 Number of audio messages to send. Default: 12");
  console.log("  --host HOST               TCP host. Default: 127.0.0.1");
  console.log("  --port PORT               TCP port. Default: 27000");
  console.log("  --timeout-ms MS           Whole-suite timeout. Default: 300000");
  console.log("  --send-gap-ms MS          Delay between socket writes. Default: 0");
  console.log("  --variant-mode MODE       unique or repeat. Default: unique");
  console.log("  --min-responses N         Minimum text responses required. Default: count");
  console.log("  --match-mode MODE         sequence or any. Default: sequence");
  console.log("  --no-start                Do not restart the Android app/server");
  process.exit(exitCode);
}

function parsePositiveInt(value, name) {
  const parsed = Number(value);
  if (!Number.isInteger(parsed) || parsed <= 0) {
    throw new Error(`${name} must be positive: ${value}`);
  }
  return parsed;
}

function parseNonNegativeInt(value, name) {
  const parsed = Number(value);
  if (!Number.isInteger(parsed) || parsed < 0) {
    throw new Error(`${name} must be non-negative: ${value}`);
  }
  return parsed;
}

function parseArgs(argv) {
  const args = {
    rootDir: process.env.STT_ROOT || DEFAULT_ROOT,
    adb: process.env.ADB || DEFAULT_ADB,
    expectedPath: null,
    host: process.env.STT_ANDROID_PRESSURE_HOST || "127.0.0.1",
    port: parsePositiveInt(process.env.STT_ANDROID_PRESSURE_PORT || String(DEFAULT_PORT), "port"),
    count: parsePositiveInt(process.env.STT_ANDROID_PRESSURE_COUNT || "12", "count"),
    minResponses: null,
    timeoutMs: parsePositiveInt(process.env.STT_ANDROID_PRESSURE_TIMEOUT_MS || "300000", "timeout-ms"),
    sendGapMs: parseNonNegativeInt(process.env.STT_ANDROID_PRESSURE_SEND_GAP_MS || "0", "send-gap-ms"),
    variantMode: process.env.STT_ANDROID_PRESSURE_VARIANT_MODE || "unique",
    matchMode: process.env.STT_ANDROID_PRESSURE_MATCH_MODE || "sequence",
    startServer: true,
    expectedBackend: process.env.STT_EXPECT_BACKEND || "sensevoice_qnn",
  };

  for (let i = 0; i < argv.length; i += 1) {
    const arg = argv[i];
    if (arg === "--help" || arg === "-h") {
      usage(0);
    } else if (arg === "--count") {
      args.count = parsePositiveInt(argv[++i], "count");
    } else if (arg === "--host") {
      args.host = argv[++i];
    } else if (arg === "--port") {
      args.port = parsePositiveInt(argv[++i], "port");
    } else if (arg === "--timeout-ms") {
      args.timeoutMs = parsePositiveInt(argv[++i], "timeout-ms");
    } else if (arg === "--min-responses") {
      args.minResponses = parsePositiveInt(argv[++i], "min-responses");
    } else if (arg === "--send-gap-ms") {
      args.sendGapMs = parseNonNegativeInt(argv[++i], "send-gap-ms");
    } else if (arg === "--variant-mode") {
      args.variantMode = argv[++i];
    } else if (arg === "--match-mode") {
      args.matchMode = argv[++i];
    } else if (arg === "--no-start") {
      args.startServer = false;
    } else if (!args.expectedPath) {
      args.expectedPath = arg;
    } else {
      throw new Error(`Unknown argument: ${arg}`);
    }
  }

  if (!args.expectedPath) {
    args.expectedPath = path.join(args.rootDir, "test_audio", "expected.tsv");
  }
  if (args.minResponses === null) {
    args.minResponses = args.count;
  }
  if (args.minResponses > args.count) {
    throw new Error(`min-responses cannot exceed count: ${args.minResponses} > ${args.count}`);
  }
  if (args.variantMode !== "unique" && args.variantMode !== "repeat") {
    throw new Error(`variant-mode must be unique or repeat: ${args.variantMode}`);
  }
  if (args.matchMode !== "sequence" && args.matchMode !== "any") {
    throw new Error(`match-mode must be sequence or any: ${args.matchMode}`);
  }
  return args;
}

function timestamp() {
  const now = new Date();
  const pad = (value) => String(value).padStart(2, "0");
  return `${now.getFullYear()}${pad(now.getMonth() + 1)}${pad(now.getDate())}-${pad(now.getHours())}${pad(now.getMinutes())}${pad(now.getSeconds())}`;
}

function append(file, text) {
  fs.appendFileSync(file, text, "utf8");
}

function run(command, args, options = {}) {
  return spawnSync(command, args, {
    cwd: options.cwd || DEFAULT_ROOT,
    encoding: "utf8",
    timeout: options.timeout,
    windowsHide: true,
  });
}

function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

function buildPressureCases(rows, count) {
  if (!Number.isInteger(count) || count <= 0) {
    throw new Error(`count must be positive: ${count}`);
  }
  const wavRows = rows.filter((row) => row.mode === "wav");
  if (wavRows.length === 0) {
    throw new Error("No wav-mode rows found in expected.tsv");
  }
  return Array.from({ length: count }, (_, index) => ({
    ...wavRows[index % wavRows.length],
    sequence: index + 1,
  }));
}

function readWav16Mono(wavPath) {
  const buffer = fs.readFileSync(wavPath);
  if (buffer.toString("ascii", 0, 4) !== "RIFF" || buffer.toString("ascii", 8, 12) !== "WAVE") {
    throw new Error(`Not a WAV file: ${wavPath}`);
  }

  const format = buffer.readUInt16LE(20);
  const channels = buffer.readUInt16LE(22);
  const sampleRate = buffer.readUInt32LE(24);
  const bitsPerSample = buffer.readUInt16LE(34);
  if (format !== 1) throw new Error(`Unsupported WAV format ${format}; expected PCM format 1`);
  if (channels !== 1) throw new Error(`Unsupported channel count ${channels}; expected mono`);
  if (bitsPerSample !== 16) throw new Error(`Unsupported bit depth ${bitsPerSample}; expected 16-bit PCM`);

  let offset = 12;
  let dataOffset = -1;
  let dataLength = 0;
  while (offset + 8 <= buffer.length) {
    const id = buffer.toString("ascii", offset, offset + 4);
    const length = buffer.readUInt32LE(offset + 4);
    if (id === "data") {
      dataOffset = offset + 8;
      dataLength = length;
      break;
    }
    offset += 8 + length + (length % 2);
  }
  if (dataOffset < 0) {
    throw new Error(`WAV data chunk not found: ${wavPath}`);
  }

  const sampleCount = Math.floor(dataLength / 2);
  const samples = new Float32Array(sampleCount);
  for (let i = 0; i < sampleCount; i += 1) {
    samples[i] = buffer.readInt16LE(dataOffset + i * 2) / 32768;
  }
  return { sampleRate, samples };
}

function applyAudioVariant(samples, sequence, variantMode) {
  if (variantMode === "repeat") {
    return Float32Array.from(samples);
  }

  const output = Float32Array.from(samples);
  if (output.length === 0) return output;

  const stride = Math.max(1600, Math.floor(output.length / 12));
  const delta = sequence * 1e-7;
  let changed = false;
  for (let i = sequence % stride; i < output.length; i += stride) {
    const before = output[i];
    const sign = output[i] >= 0 ? 1 : -1;
    output[i] = Math.max(-1, Math.min(1, output[i] + sign * delta));
    if (output[i] !== before) changed = true;
  }

  if (!changed) {
    const fallback = output.findIndex((value) => Math.abs(value) < 0.9);
    const index = fallback >= 0 ? fallback : sequence % output.length;
    const before = output[index];
    const sign = output[index] >= 0 ? 1 : -1;
    output[index] = Math.max(-1, Math.min(1, output[index] + sign * delta));
    if (output[index] === before && output[index] > 0) {
      output[index] = Math.max(-1, output[index] - delta);
    }
  }
  return output;
}

function makeAudioPacket(samples, sampleRate, segmentId = 0) {
  const payloadLength = 4 + 2 + samples.length * 4 + 4;
  const buffer = Buffer.alloc(8 + payloadLength);
  let offset = 0;
  buffer.writeUInt16BE(MAGIC, offset);
  offset += 2;
  buffer.writeUInt8(MESSAGE_TYPE_AUDIO, offset);
  offset += 1;
  buffer.writeUInt8(0x01 | FLAG_HAS_SEGMENT_ID, offset);
  offset += 1;
  buffer.writeUInt32BE(payloadLength, offset);
  offset += 4;
  buffer.writeUInt32BE(sampleRate, offset);
  offset += 4;
  buffer.writeUInt16BE(1, offset);
  offset += 2;
  for (let i = 0; i < samples.length; i += 1, offset += 4) {
    buffer.writeFloatLE(samples[i], offset);
  }
  buffer.writeUInt32BE(segmentId, offset);
  return buffer;
}

function parseTextPayload(payload, flags = 0) {
  if (payload.length < 4) {
    throw new Error(`Text payload too short: ${payload.length}`);
  }
  let offset = 0;
  const textLength = payload.readUInt32BE(offset);
  offset += 4;
  if (offset + textLength > payload.length) {
    throw new Error(`Invalid text payload length: ${textLength}`);
  }
  const text = payload.subarray(offset, offset + textLength).toString("utf8");
  offset += textLength;
  let segmentId = null;
  const emotion = offset < payload.length ? payload.readUInt8(offset) : "";
  if (offset < payload.length) offset += 1;
  const event = offset < payload.length ? payload.readUInt8(offset) : "";
  if (offset < payload.length) offset += 1;

  if ((flags & FLAG_HAS_SEGMENT_ID) && offset + 4 <= payload.length) {
    segmentId = payload.readUInt32BE(offset);
  }

  return {
    text,
    emotion,
    event,
    segmentId,
  };
}

function evaluateResponse({ caseItem, text, responseMs, sincePreviousResponseMs, segmentId = null, emotion = "", event = "" }) {
  const expected = caseItem.expected_text || caseItem.expected_contains || "";
  const expectedContains = caseItem.expected_contains || "";
  const ok = textMatchesExpectation(text, expectedContains);
  return {
    sequence: caseItem.sequence,
    file: caseItem.file,
    ok,
    expected,
    expectedContains,
    actual: text,
    responseMs,
    sincePreviousResponseMs,
    segmentId,
    cer: characterErrorRate(expected, text),
    reason: ok ? "pass" : "expected substring missing",
    emotion,
    event,
  };
}

function evaluateAnyResponse({ cases, responseIndex, text, responseMs, sincePreviousResponseMs, segmentId = null, emotion = "", event = "" }) {
  const matched = cases.find((caseItem) => textMatchesExpectation(text, caseItem.expected_contains || ""));
  if (matched) {
    return evaluateResponse({
      caseItem: { ...matched, sequence: responseIndex },
      text,
      responseMs,
      sincePreviousResponseMs,
      segmentId,
      emotion,
      event,
    });
  }

  const fallback = cases[(responseIndex - 1) % cases.length];
  return {
    ...evaluateResponse({
      caseItem: { ...fallback, sequence: responseIndex },
      text,
      responseMs,
      sincePreviousResponseMs,
      segmentId,
      emotion,
      event,
    }),
    ok: false,
    reason: "no expected substring matched",
  };
}

function cleanCell(value) {
  return String(value ?? "").replace(/\r?\n/g, " ").replace(/\t/g, " ").trim();
}

function formatPressureTsv(rows) {
  const header = [
    "sequence",
    "segment_id",
    "file",
    "ok",
    "response_ms",
    "since_previous_response_ms",
    "cer",
    "expected_contains",
    "actual",
    "emotion",
    "event",
    "reason",
  ];
  const lines = [header.join("\t")];
  for (const row of rows) {
    lines.push([
      row.sequence,
      row.segmentId,
      row.file,
      row.ok ? "pass" : "fail",
      row.responseMs,
      row.sincePreviousResponseMs,
      row.cer.toFixed(3),
      row.expectedContains,
      row.actual,
      row.emotion,
      row.event,
      row.reason,
    ].map(cleanCell).join("\t"));
  }
  return `${lines.join("\n")}\n`;
}

function appendAndroidLog(adb, resultFile) {
  const result = run(adb, [
    "logcat",
    "-d",
    "-s",
    "STT_Native",
    "STT_Engine",
    "sherpa-onnx",
    "QnnDsp",
    "QnnHtp",
    "QnnDspTransport",
  ]);
  append(resultFile, "\n[Android log]\n");
  const lines = result.stdout
    .split(/\r?\n/)
    .filter((line) => (
      line.includes("Audio cb") ||
      line.includes("Recognize OK in") ||
      line.includes("Recognize cache hit") ||
      line.includes("Audio queue full") ||
      line.includes("Sent response") ||
      line.includes("Sent cached response") ||
      line.includes("Recognizer backend") ||
      line.includes("Server listening on port") ||
      line.includes("Selected backend") ||
      line.includes("Failed to ") ||
      line.includes("Client disconnected")
    ));
  append(resultFile, `${lines.join("\n")}\n`);
  return lines.join("\n");
}

function waitServer(adb, resultFile, expectedBackend, timeoutMs) {
  const intervalMs = 500;
  const attempts = Math.max(1, Math.ceil(timeoutMs / intervalMs));
  for (let i = 0; i < attempts; i += 1) {
    const result = run(adb, ["logcat", "-d", "-s", "STT_Native", "STT_Engine", "sherpa-onnx"]);
    const ready = result.stdout.includes("Server listening on port 27000");
    const backendReady = !expectedBackend || result.stdout.includes(`Recognizer backend: ${expectedBackend}`);
    if (ready && backendReady) {
      return true;
    }
    Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, intervalMs);
  }
  append(resultFile, `[FAIL] Android server did not become ready within ${timeoutMs} ms\n`);
  appendAndroidLog(adb, resultFile);
  return false;
}

function startAndroidServer(adb, resultFile, expectedBackend) {
  if (!fs.existsSync(adb)) {
    append(resultFile, `[FAIL] adb not found: ${adb}\n`);
    return false;
  }

  const devices = run(adb, ["devices"]);
  append(resultFile, `${devices.stdout}${devices.stderr}`);
  if (!devices.stdout.split(/\r?\n/).some((line) => /\tdevice$/.test(line))) {
    append(resultFile, "[FAIL] no connected Android device\n");
    return false;
  }

  const commands = [
    ["logcat", "-c"],
    ["shell", "input", "keyevent", "KEYCODE_WAKEUP"],
    ["shell", "wm", "dismiss-keyguard"],
    ["shell", "am", "force-stop", PACKAGE],
    ["shell", "am", "start", "-n", APP, "--ez", "autoStart", "true"],
    ["forward", `tcp:${DEFAULT_PORT}`, `tcp:${DEFAULT_PORT}`],
  ];
  for (const command of commands) {
    const result = run(adb, command);
    append(resultFile, `${result.stdout}${result.stderr}`);
    if (result.status !== 0) {
      append(resultFile, `[FAIL] adb ${command.join(" ")} failed with exit ${result.status}\n`);
      return false;
    }
  }

  return waitServer(adb, resultFile, expectedBackend, 90000);
}

async function runPressureConnection({ host, port, cases, rootDir, timeoutMs, sendGapMs, variantMode, matchMode, resultFile }) {
  const startedAt = Date.now();
  const rows = [];
  let lastResponseAt = 0;
  let receiveBuffer = Buffer.alloc(0);
  let finished = false;
  let socketRef = null;

  const prepared = cases.map((caseItem) => {
    const wavPath = path.resolve(rootDir, caseItem.file);
    if (!fs.existsSync(wavPath)) {
      throw new Error(`Missing WAV: ${wavPath}`);
    }
    const wav = readWav16Mono(wavPath);
    const samples = applyAudioVariant(wav.samples, caseItem.sequence, variantMode);
    const durationMs = Math.round((samples.length * 1000) / wav.sampleRate);
    return {
      caseItem,
      wavPath,
      packet: makeAudioPacket(samples, wav.sampleRate, caseItem.sequence),
      durationMs,
      sentAt: 0,
    };
  });

  return new Promise((resolve, reject) => {
    const timeout = setTimeout(() => {
      if (finished) return;
      finished = true;
      if (socketRef) socketRef.end();
      append(resultFile, `[TIMEOUT] ${rows.length}/${prepared.length} responses received after ${timeoutMs} ms\n`);
      resolve({
        rows,
        totalElapsedMs: Date.now() - startedAt,
        timedOut: true,
      });
    }, timeoutMs);

    function complete() {
      if (finished) return;
      finished = true;
      clearTimeout(timeout);
      if (socketRef) socketRef.end();
      resolve({
        rows,
        totalElapsedMs: Date.now() - startedAt,
        timedOut: false,
      });
    }

    function fail(error) {
      if (finished) return;
      finished = true;
      clearTimeout(timeout);
      if (socketRef) socketRef.destroy();
      reject(error);
    }

    function handlePacket(type, flags, payload) {
      if (type === MESSAGE_TYPE_ERROR) {
        fail(new Error(`Android returned error payload: ${payload.toString("utf8")}`));
        return;
      }
      if (type !== MESSAGE_TYPE_TEXT) {
        append(resultFile, `[WARN] Ignored message type=${type} payload=${payload.length} bytes\n`);
        return;
      }

      const caseIndex = rows.length;
      if (caseIndex >= prepared.length) {
        fail(new Error("Received more text responses than sent audio messages"));
        return;
      }

      const parsed = parseTextPayload(payload, flags);
      const now = Date.now();
      const sentIndex = parsed.segmentId ? prepared.findIndex((item) => item.caseItem.sequence === parsed.segmentId) : caseIndex;
      const sentAt = sentIndex >= 0 ? (prepared[sentIndex].sentAt || startedAt) : startedAt;
      const row = matchMode === "any" ? evaluateAnyResponse({
        cases,
        responseIndex: parsed.segmentId ?? rows.length + 1,
        text: parsed.text,
        responseMs: now - sentAt,
        sincePreviousResponseMs: lastResponseAt ? now - lastResponseAt : 0,
        segmentId: parsed.segmentId,
        emotion: parsed.emotion,
        event: parsed.event,
      }) : evaluateResponse({
        caseItem: parsed.segmentId && sentIndex >= 0 ? prepared[sentIndex].caseItem : prepared[caseIndex].caseItem,
        text: parsed.text,
        responseMs: now - sentAt,
        sincePreviousResponseMs: lastResponseAt ? now - lastResponseAt : 0,
        segmentId: parsed.segmentId,
        emotion: parsed.emotion,
        event: parsed.event,
      });
      lastResponseAt = now;
      rows.push(row);

      append(
        resultFile,
        `[RESPONSE ${row.sequence}] segment=${row.segmentId ?? ""} ok=${row.ok ? "pass" : "fail"} response_ms=${row.responseMs} ` +
          `since_previous_ms=${row.sincePreviousResponseMs} text=${row.actual}\n`,
      );

      if (rows.length === prepared.length) {
        complete();
      }
    }

    const socket = net.createConnection({ host, port });
    socketRef = socket;

    socket.on("connect", async () => {
      append(resultFile, `[CONNECT] ${host}:${port}\n`);
      try {
        for (const item of prepared) {
          item.sentAt = Date.now();
          append(
            resultFile,
            `[SEND ${item.caseItem.sequence}] ${item.caseItem.file} audio_ms=${item.durationMs} ` +
              `payload=${item.packet.length - 8} bytes\n`,
          );
          socket.write(item.packet);
          if (sendGapMs > 0) {
            await sleep(sendGapMs);
          }
        }
      } catch (error) {
        fail(error);
      }
    });

    socket.on("data", (chunk) => {
      receiveBuffer = Buffer.concat([receiveBuffer, chunk]);
      while (receiveBuffer.length >= 8) {
        const magic = receiveBuffer.readUInt16BE(0);
        const type = receiveBuffer.readUInt8(2);
        const flags = receiveBuffer.readUInt8(3);
        const length = receiveBuffer.readUInt32BE(4);
        if (receiveBuffer.length < 8 + length) return;

        const payload = receiveBuffer.subarray(8, 8 + length);
        receiveBuffer = receiveBuffer.subarray(8 + length);
        if (magic !== MAGIC) {
          fail(new Error(`Invalid magic: 0x${magic.toString(16)}`));
          return;
        }
        handlePacket(type, flags, payload);
      }
    });

    socket.on("error", fail);
    socket.on("close", () => {
      if (!finished) {
        fail(new Error(`Socket closed before all responses arrived: ${rows.length}/${prepared.length}`));
      }
    });
  });
}

function summarizeRows(rows, totalElapsedMs, sentCount = rows.length) {
  const passed = rows.filter((row) => row.ok).length;
  const responseTimes = rows.map((row) => row.responseMs);
  const maxResponseMs = responseTimes.length ? Math.max(...responseTimes) : 0;
  const avgResponseMs = responseTimes.length
    ? Math.round(responseTimes.reduce((sum, value) => sum + value, 0) / responseTimes.length)
    : 0;
  return {
    passed,
    total: rows.length,
    sent: sentCount,
    droppedOrMissing: Math.max(0, sentCount - rows.length),
    totalElapsedMs,
    maxResponseMs,
    avgResponseMs,
  };
}

function pressureExitCode(summary, args) {
  return summary.total >= args.minResponses && summary.passed === summary.total ? 0 : 1;
}

async function main() {
  const args = parseArgs(process.argv.slice(2));
  const resultDir = path.join(args.rootDir, "build", "test-results");
  fs.mkdirSync(resultDir, { recursive: true });
  const runId = timestamp();
  const resultFile = path.join(resultDir, `android-pressure-${runId}.txt`);
  const tsvFile = path.join(resultDir, `android-pressure-${runId}.tsv`);

  const rows = parseExpectedTsv(fs.readFileSync(args.expectedPath, "utf8"));
  const cases = buildPressureCases(rows, args.count);

  append(resultFile, "============================================================\n");
  append(resultFile, "  Android pressure suite\n");
  append(resultFile, `  ${new Date().toISOString()}\n`);
  append(resultFile, `  expected: ${args.expectedPath}\n`);
  append(resultFile, `  count: ${args.count}\n`);
  append(resultFile, `  min_responses: ${args.minResponses}\n`);
  append(resultFile, `  variant_mode: ${args.variantMode}\n`);
  append(resultFile, `  match_mode: ${args.matchMode}\n`);
  append(resultFile, `  send_gap_ms: ${args.sendGapMs}\n`);
  append(resultFile, `  timeout_ms: ${args.timeoutMs}\n`);
  append(resultFile, `  start_server: ${args.startServer ? "yes" : "no"}\n`);
  append(resultFile, "============================================================\n");

  console.log(`Results: ${resultFile}`);

  if (args.startServer && !startAndroidServer(args.adb, resultFile, args.expectedBackend)) {
    console.log("FAIL: Android server did not become ready");
    process.exit(1);
  }

  let pressure;
  try {
    pressure = await runPressureConnection({
      host: args.host,
      port: args.port,
      cases,
      rootDir: args.rootDir,
      timeoutMs: args.timeoutMs,
      sendGapMs: args.sendGapMs,
      variantMode: args.variantMode,
      matchMode: args.matchMode,
      resultFile,
    });
  } catch (error) {
    append(resultFile, `[FAIL] ${error.message}\n`);
    appendAndroidLog(args.adb, resultFile);
    console.log(`FAIL: ${error.message}`);
    console.log(`Results: ${resultFile}`);
    process.exit(1);
  }

  appendAndroidLog(args.adb, resultFile);
  fs.writeFileSync(tsvFile, formatPressureTsv(pressure.rows), "utf8");

  const summary = summarizeRows(pressure.rows, pressure.totalElapsedMs, args.count);
  append(resultFile, "\n[Summary]\n");
  append(resultFile, `Summary: ${summary.passed}/${summary.total} responses passed\n`);
  append(resultFile, `Sent: ${summary.sent}\n`);
  append(resultFile, `Dropped or missing: ${summary.droppedOrMissing}\n`);
  append(resultFile, `Total elapsed: ${summary.totalElapsedMs} ms\n`);
  append(resultFile, `Average response: ${summary.avgResponseMs} ms\n`);
  append(resultFile, `Max response: ${summary.maxResponseMs} ms\n`);
  append(resultFile, `TSV: ${tsvFile}\n`);

  console.log(`Summary: ${summary.passed}/${summary.total} responses passed`);
  console.log(`Sent: ${summary.sent}`);
  console.log(`Dropped or missing: ${summary.droppedOrMissing}`);
  console.log(`Total elapsed: ${summary.totalElapsedMs} ms`);
  console.log(`Average response: ${summary.avgResponseMs} ms`);
  console.log(`Max response: ${summary.maxResponseMs} ms`);
  console.log(`TSV: ${tsvFile}`);

  process.exit(pressureExitCode(summary, args));
}

if (require.main === module) {
  main().catch((error) => {
    console.error(`[Error] ${error.message}`);
    process.exit(1);
  });
}

module.exports = {
  applyAudioVariant,
  buildPressureCases,
  evaluateAnyResponse,
  evaluateResponse,
  formatPressureTsv,
  makeAudioPacket,
  parseTextPayload,
  parsePositiveInt,
  pressureExitCode,
  summarizeRows,
};
