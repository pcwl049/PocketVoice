#!/usr/bin/env node

const fs = require("fs");
const path = require("path");
const { spawnSync } = require("child_process");

const DEFAULT_ROOT = path.resolve(__dirname, "..");
const DEFAULT_ADB = "D:\\Android\\Sdk\\platform-tools\\adb.exe";
const APP = "com.stt.mobile/.MainActivity";
const PACKAGE = "com.stt.mobile";
const PORT = "27000";

function parseExpectedTsv(content) {
  const lines = content.split(/\r?\n/).filter((line) => line.trim() && !line.trim().startsWith("#"));
  if (lines.length === 0) return [];

  const headers = lines[0].split("\t");
  const required = ["file", "mode", "expected_contains"];
  for (const name of required) {
    if (!headers.includes(name)) {
      throw new Error(`Missing expected.tsv column: ${name}`);
    }
  }

  return lines.slice(1).map((line, index) => {
    const values = line.split("\t");
    const row = {};
    headers.forEach((header, i) => {
      row[header] = values[i] || "";
    });
    if (!row.file || !row.mode) {
      throw new Error(`Invalid expected.tsv row ${index + 2}: file and mode are required`);
    }
    if (row.mode !== "wav" && row.mode !== "wav-vad" && row.mode !== "simulate-wav-vad") {
      throw new Error(`Invalid mode on row ${index + 2}: ${row.mode}`);
    }
    return row;
  });
}

function parsePcText(output) {
  const matches = [...output.matchAll(/^\[Text\]\s*(.*)$/gm)];
  if (matches.length === 0) return "";
  return matches[matches.length - 1][1].trim();
}

function parseTimingMs(output) {
  const matches = [...output.matchAll(/^\[Timing\].*:\s*(\d+)\s*ms$/gm)];
  if (matches.length === 0) return "";
  return matches[matches.length - 1][1];
}

function parseDryRunText(output) {
  const matches = [...output.matchAll(/^\[ChatBox dry-run\]\s*(.*)$/gm)];
  if (matches.length === 0) return "";
  return matches.map((match) => match[1].trim()).join("");
}

function textMatchesExpectation(text, expectedContains) {
  if (!expectedContains) return true;
  return text.includes(expectedContains);
}

function characterErrorRate(expected, actual) {
  if (!expected) return actual ? 1 : 0;
  const a = [...expected];
  const b = [...actual];
  const dp = Array.from({ length: a.length + 1 }, () => Array(b.length + 1).fill(0));
  for (let i = 0; i <= a.length; i += 1) dp[i][0] = i;
  for (let j = 0; j <= b.length; j += 1) dp[0][j] = j;
  for (let i = 1; i <= a.length; i += 1) {
    for (let j = 1; j <= b.length; j += 1) {
      const cost = a[i - 1] === b[j - 1] ? 0 : 1;
      dp[i][j] = Math.min(
        dp[i - 1][j] + 1,
        dp[i][j - 1] + 1,
        dp[i - 1][j - 1] + cost,
      );
    }
  }
  return dp[a.length][b.length] / a.length;
}

function cleanCell(value) {
  return String(value ?? "").replace(/\r?\n/g, " ").replace(/\t/g, " ").trim();
}

function markdownCell(value) {
  return cleanCell(value).replace(/\|/g, "\\|");
}

function formatAccuracyTsv(rows) {
  const header = ["file", "mode", "ok", "cer", "elapsed_ms", "expected", "actual", "chatbox", "reason"];
  const lines = [header.join("\t")];
  for (const row of rows) {
    lines.push([
      row.file,
      row.mode,
      row.ok ? "pass" : "fail",
      row.cer.toFixed(3),
      row.elapsedMs,
      row.expected,
      row.actual,
      row.dryRunText,
      row.reason,
    ].map(cleanCell).join("\t"));
  }
  return `${lines.join("\n")}\n`;
}

function formatAccuracyMarkdown(rows) {
  const lines = [
    "# PocketVoice Accuracy Baseline",
    "",
    "| file | mode | ok | CER | elapsed_ms | expected | actual |",
    "|---|---|---:|---:|---:|---|---|",
  ];
  for (const row of rows) {
    lines.push(`| ${markdownCell(row.file)} | ${markdownCell(row.mode)} | ${row.ok ? "pass" : "fail"} | ${row.cer.toFixed(3)} | ${markdownCell(row.elapsedMs)} | ${markdownCell(row.expected)} | ${markdownCell(row.actual)} |`);
  }
  lines.push("");
  return `${lines.join("\n")}\n`;
}

function androidLogContainsBackend(logText, backendName) {
  if (!backendName) return true;
  return logText.includes(`Recognizer backend: ${backendName}`);
}

function timestamp() {
  const now = new Date();
  const pad = (value) => String(value).padStart(2, "0");
  return `${now.getFullYear()}${pad(now.getMonth() + 1)}${pad(now.getDate())}-${pad(now.getHours())}${pad(now.getMinutes())}${pad(now.getSeconds())}`;
}

function run(command, args, options = {}) {
  return spawnSync(command, args, {
    cwd: options.cwd || DEFAULT_ROOT,
    encoding: "utf8",
    timeout: options.timeout,
    windowsHide: true,
  });
}

function append(file, text) {
  fs.appendFileSync(file, text, "utf8");
}

function serverStartAttempts() {
  const attempts = Number(process.env.STT_SERVER_START_ATTEMPTS || "2");
  if (!Number.isFinite(attempts) || attempts < 1) {
    return 1;
  }
  return Math.floor(attempts);
}

function waitServer(adb, resultFile) {
  const timeoutMs = Number(process.env.STT_SERVER_READY_TIMEOUT_MS || "90000");
  const intervalMs = 500;
  const attempts = Math.max(1, Math.ceil(timeoutMs / intervalMs));
  for (let i = 0; i < attempts; i += 1) {
    const result = run(adb, ["logcat", "-d", "-s", "STT_Native", "STT_Network"]);
    if (result.stdout.includes("Server listening on port 27000")) {
      return true;
    }
    Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, intervalMs);
  }
  append(resultFile, "[FAIL] Server did not become ready\n");
  appendAndroidLog(adb, resultFile);
  return false;
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
      line.includes("Recognizer backend") ||
      line.includes("Server listening on port") ||
      line.includes("Selected backend") ||
      line.includes("QNN model lib") ||
      line.includes("QNN context binary") ||
      line.includes("Creating offline QNN recognizer") ||
      line.includes("loaded ") ||
      line.includes("graphs_count") ||
      line.includes("Finalizing graph") ||
      line.includes("input ") ||
      line.includes("output ") ||
      line.includes("Return code is") ||
      line.includes("Failed to ") ||
      line.includes("Could not create context from binary") ||
      line.includes("Expect two input tensors")
    ));
  append(resultFile, `${lines.join("\n")}\n`);
  return lines.join("\n");
}

function startSuiteAndroidServer(adb, resultFile) {
  for (const args of [
    ["shell", "am", "force-stop", PACKAGE],
    ["logcat", "-c"],
    ["shell", "am", "start", "-n", APP],
  ]) {
    const result = run(adb, args);
    append(resultFile, `${result.stdout}${result.stderr}`);
    if (result.status !== 0) return false;
  }

  Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, 1000);
  run(adb, ["shell", "input", "keyevent", "61"]);
  Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, 1000);
  run(adb, ["shell", "input", "keyevent", "66"]);

  const forward = run(adb, ["forward", `tcp:${PORT}`, `tcp:${PORT}`]);
  append(resultFile, `${forward.stdout}${forward.stderr}`);
  if (forward.status !== 0) return false;

  return waitServer(adb, resultFile);
}

function runCase({ rootDir, adb, resultFile, row }) {
  const wavPath = path.resolve(rootDir, row.file);
  const exe = path.join(rootDir, "build", "pc", "stt_pc.exe");

  append(resultFile, "\n------------------------------------------------------------\n");
  append(resultFile, `CASE ${row.mode} ${wavPath}\n`);
  append(resultFile, "------------------------------------------------------------\n");

  if (!fs.existsSync(wavPath)) {
    append(resultFile, `[FAIL] Missing WAV: ${wavPath}\n`);
    return { ok: false, reason: "missing wav" };
  }

  let args;
  if (row.mode === "wav") {
    args = ["--wav", wavPath, "--chatbox-dry-run"];
  } else if (row.mode === "wav-vad") {
    args = ["--wav-vad", wavPath, "--chatbox-dry-run"];
  } else {
    args = ["--simulate-wav-vad", wavPath, "--chatbox-dry-run"];
  }
  const pcTimeoutMs = Number(process.env.STT_PC_CASE_TIMEOUT_MS || "30000");
  const result = run(exe, args, { cwd: rootDir, timeout: pcTimeoutMs });
  const output = `${result.stdout}${result.stderr}`;
  append(resultFile, output);
  append(resultFile, `Exit: ${result.status}\n`);
  const androidLog = appendAndroidLog(adb, resultFile);

  const text = parsePcText(output);
  const dryRunText = parseDryRunText(output);
  const expected = row.expected_text || row.expected_contains || "";
  const elapsedMs = parseTimingMs(output);
  const baseAccuracy = {
    file: row.file,
    mode: row.mode,
    expected,
    actual: text,
    dryRunText,
    elapsedMs,
    cer: characterErrorRate(expected, text),
  };
  const textOk = textMatchesExpectation(text, row.expected_contains);
  const expectedBackend = process.env.STT_EXPECT_BACKEND || "";
  if (result.status !== 0) {
    append(resultFile, `[FAIL] stt_pc exit code ${result.status}\n`);
    return { ok: false, reason: `exit ${result.status}`, accuracy: { ...baseAccuracy, ok: false, reason: `exit ${result.status}` } };
  }
  if (!androidLogContainsBackend(androidLog, expectedBackend)) {
    append(resultFile, `[FAIL] Expected Android backend missing: ${expectedBackend}\n`);
    return { ok: false, reason: "expected backend missing", accuracy: { ...baseAccuracy, ok: false, reason: "expected backend missing" } };
  }
  if (!textOk) {
    append(resultFile, `[FAIL] Expected substring missing: ${row.expected_contains}\n`);
    append(resultFile, `[FAIL] Actual text: ${text}\n`);
    return { ok: false, reason: "expected substring missing", accuracy: { ...baseAccuracy, ok: false, reason: "expected substring missing" } };
  }
  if (!dryRunText) {
    append(resultFile, `[FAIL] ChatBox dry-run output missing\n`);
    append(resultFile, `[FAIL] Dry-run text: ${dryRunText}\n`);
    return { ok: false, reason: "dry-run output missing", accuracy: { ...baseAccuracy, ok: false, reason: "dry-run output missing" } };
  }

  append(resultFile, `[PASS] Text contains: ${row.expected_contains}\n`);
  append(resultFile, `[PASS] ChatBox dry-run produced output\n`);
  return { ok: true, reason: "pass", accuracy: { ...baseAccuracy, ok: true, reason: "pass" } };
}

function main() {
  const rootDir = process.env.STT_ROOT || DEFAULT_ROOT;
  const adb = process.env.ADB || DEFAULT_ADB;
  const expectedPath = process.argv[2] || path.join(rootDir, "test_audio", "expected.tsv");
  const resultDir = path.join(rootDir, "build", "test-results");
  fs.mkdirSync(resultDir, { recursive: true });
  const runId = timestamp();
  const resultFile = path.join(resultDir, `pc-wav-suite-${runId}.txt`);
  const accuracyTsvFile = path.join(resultDir, `accuracy-baseline-${runId}.tsv`);
  const accuracyMdFile = path.join(resultDir, `accuracy-baseline-${runId}.md`);

  const rows = parseExpectedTsv(fs.readFileSync(expectedPath, "utf8"));
  append(resultFile, "============================================================\n");
  append(resultFile, "  PC WAV regression suite\n");
  append(resultFile, `  ${new Date().toISOString()}\n`);
  append(resultFile, `  Expected: ${expectedPath}\n`);
  append(resultFile, "============================================================\n");

  console.log(`Results: ${resultFile}`);

  let serverReady = false;
  const attempts = serverStartAttempts();
  for (let attempt = 1; attempt <= attempts; attempt += 1) {
    if (startSuiteAndroidServer(adb, resultFile)) {
      serverReady = true;
      break;
    }
    if (attempt < attempts) {
      append(resultFile, `[RETRY] Server start attempt ${attempt}/${attempts} failed; retrying\n`);
    }
  }
  if (!serverReady) {
    append(resultFile, "[FAIL] Android server did not become ready before suite cases\n");
    console.log("FAIL: Android server did not become ready before suite cases");
    process.exit(1);
  }

  let failures = 0;
  const accuracyRows = [];
  for (const row of rows) {
    process.stdout.write(`[CASE] ${row.mode} ${row.file} ... `);
    const caseResult = runCase({ rootDir, adb, resultFile, row });
    if (caseResult.accuracy) accuracyRows.push(caseResult.accuracy);
    if (caseResult.ok) {
      console.log("PASS");
    } else {
      failures += 1;
      console.log(`FAIL (${caseResult.reason})`);
    }
  }

  append(resultFile, `\nSummary: ${rows.length - failures}/${rows.length} passed\n`);
  fs.writeFileSync(accuracyTsvFile, formatAccuracyTsv(accuracyRows), "utf8");
  fs.writeFileSync(accuracyMdFile, formatAccuracyMarkdown(accuracyRows), "utf8");
  append(resultFile, `Accuracy TSV: ${accuracyTsvFile}\n`);
  append(resultFile, `Accuracy MD: ${accuracyMdFile}\n`);
  console.log(`Summary: ${rows.length - failures}/${rows.length} passed`);
  console.log(`Results: ${resultFile}`);
  console.log(`Accuracy TSV: ${accuracyTsvFile}`);
  console.log(`Accuracy MD: ${accuracyMdFile}`);
  process.exit(failures === 0 ? 0 : 1);
}

if (require.main === module) {
  main();
}

module.exports = {
  parseExpectedTsv,
  parseDryRunText,
  parsePcText,
  parseTimingMs,
  characterErrorRate,
  formatAccuracyTsv,
  formatAccuracyMarkdown,
  androidLogContainsBackend,
  serverStartAttempts,
  textMatchesExpectation,
};
