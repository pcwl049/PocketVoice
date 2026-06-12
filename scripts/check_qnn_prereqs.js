#!/usr/bin/env node

const fs = require("fs");
const path = require("path");
const { spawnSync } = require("child_process");

const root = process.env.STT_ROOT || path.resolve(__dirname, "..");
const androidHome = process.env.ANDROID_HOME || process.env.ANDROID_SDK_ROOT || "";
const adb = process.env.ADB || (androidHome ? path.join(androidHome, "platform-tools", "adb.exe") : "adb");
const qnnSdk = process.env.QNN_SDK_ROOT || process.env.QNN_SDK || "";
const senseVoiceDir = path.join(root, "models", "sensevoice");

let failures = 0;
let warnings = 0;

function ok(label, value) {
  console.log(`[OK] ${label}: ${value}`);
}

function warn(label, value) {
  warnings += 1;
  console.log(`[WARN] ${label}: ${value}`);
}

function fail(label, value) {
  failures += 1;
  console.log(`[FAIL] ${label}: ${value}`);
}

function requireDir(filePath, label) {
  if (fs.existsSync(filePath) && fs.statSync(filePath).isDirectory()) {
    ok(label, filePath);
    return true;
  }
  fail(label, filePath);
  return false;
}

function requireFile(filePath, label) {
  if (fs.existsSync(filePath) && fs.statSync(filePath).isFile()) {
    ok(label, filePath);
    return true;
  }
  fail(label, filePath);
  return false;
}

function optionalFile(filePath, label) {
  if (fs.existsSync(filePath) && fs.statSync(filePath).isFile()) {
    ok(label, filePath);
    return true;
  }
  warn(label, `${filePath} not present`);
  return false;
}

function run(command, args) {
  return spawnSync(command, args, {
    cwd: root,
    encoding: "utf8",
    windowsHide: true,
  });
}

function readDeviceSoc() {
  if (!fs.existsSync(adb)) {
    fail("ADB", adb);
    return "";
  }
  ok("ADB", adb);

  const devices = run(adb, ["devices"]);
  if (devices.status !== 0) {
    fail("ADB devices", `${devices.stdout}${devices.stderr}`.trim());
    return "";
  }

  const lines = devices.stdout
    .split(/\r?\n/)
    .map((line) => line.trim())
    .filter((line) => line.endsWith("\tdevice"));
  if (lines.length === 0) {
    fail("Android device", "no connected device reported by adb devices");
    return "";
  }
  ok("Android device", lines[0].split(/\s+/)[0]);

  const soc = run(adb, ["shell", "getprop", "ro.soc.model"]);
  if (soc.status !== 0) {
    fail("ro.soc.model", `${soc.stdout}${soc.stderr}`.trim());
    return "";
  }

  const value = soc.stdout.trim().toUpperCase();
  if (!value) {
    fail("ro.soc.model", "empty");
    return "";
  }
  ok("ro.soc.model", value);
  return value;
}

function readQnnSocId(soc) {
  const header = path.join(qnnSdk, "include", "QNN", "QnnTypes.h");
  if (!fs.existsSync(header)) {
    fail("QnnTypes.h", header);
    return "";
  }
  const text = fs.readFileSync(header, "utf8");
  const match = text.match(new RegExp(`QNN_SOC_MODEL_${soc}\\s*=\\s*(\\d+)`));
  if (!match) {
    warn("QNN SoC enum", `${soc} was not found in ${header}`);
    return "";
  }
  ok("QNN SoC enum", `QNN_SOC_MODEL_${soc} = ${match[1]}`);
  return match[1];
}

function inferHtpArch(deviceSoc) {
  const archBySoc = new Map([
    ["SA8255", "V73"],
    ["SA8295", "V68"],
    ["SM8350", "V68"],
    ["SM8450", "V69"],
    ["SM8475", "V69"],
    ["SM8550", "V73"],
    ["SM8650", "V75"],
    ["SM8735", "V79"],
    ["SM8750", "V79"],
    ["SM8850", "V81"],
    ["QCS9100", "V73"],
  ]);
  return archBySoc.get(deviceSoc) || "";
}

function checkHtpArchLibraries(deviceSoc) {
  const arch = inferHtpArch(deviceSoc);
  if (!arch) {
    warn(
      "HTP architecture",
      `${deviceSoc || "unknown device"} has no local arch mapping; copied multi-arch HTP libraries will be used`,
    );
    return;
  }

  ok("HTP architecture", arch);
  for (const lib of [
    `libQnnHtp${arch}Stub.so`,
    `libQnnHtp${arch}CalculatorStub.so`,
  ]) {
    requireFile(path.join(qnnSdk, "lib", "aarch64-android", lib), lib);
  }
  const hexagonArch = `hexagon-${arch.toLowerCase()}`;
  const skel = `libQnnHtp${arch}Skel.so`;
  requireFile(path.join(qnnSdk, "lib", hexagonArch, "unsigned", skel), skel);
}

function inferModelTargets() {
  const targets = new Set();
  const explicit = (process.env.STT_QNN_MODEL_TARGET || "").trim().toUpperCase();
  if (explicit) targets.add(explicit);

  if (!fs.existsSync(senseVoiceDir)) return [...targets];

  for (const entry of fs.readdirSync(senseVoiceDir, { withFileTypes: true })) {
    const name = entry.name.toUpperCase();
    const matches = [...name.matchAll(/\bSM\d{4}\b/g)];
    for (const match of matches) targets.add(match[0]);
  }

  const infoPath = path.join(senseVoiceDir, "info.txt");
  if (fs.existsSync(infoPath)) {
    const info = fs.readFileSync(infoPath, "utf8").toUpperCase();
    const matches = [...info.matchAll(/\bSM\d{4}\b/g)];
    for (const match of matches) targets.add(match[0]);
  }

  return [...targets].sort();
}

function hasGeneratorInput() {
  const entries = fs.existsSync(senseVoiceDir)
    ? fs.readdirSync(senseVoiceDir, { withFileTypes: true })
    : [];
  return entries.some((entry) => {
    const name = entry.name.toLowerCase();
    return entry.isFile() && (name.endsWith(".so") || name.endsWith(".dlc"));
  });
}

function checkModelTarget(deviceSoc) {
  const hasContext = fs.existsSync(path.join(senseVoiceDir, "model.bin"));
  const canGenerateOnDevice = hasGeneratorInput();
  if (!hasContext && !canGenerateOnDevice) {
    warn(
      "SenseVoice QNN model target",
      "skipped because no active model.bin or model .so/.dlc is present",
    );
    return;
  }

  const targets = inferModelTargets();
  if (targets.length === 0) {
    warn(
      "SenseVoice QNN model target",
      "no SMxxxx marker found; set STT_QNN_MODEL_TARGET to make this check strict",
    );
    return;
  }

  ok("SenseVoice QNN model target", targets.join(", "));
  if (deviceSoc && !targets.includes(deviceSoc)) {
    if (canGenerateOnDevice) {
      warn(
        "QNN context target",
        `prebuilt target ${targets.join(", ")} does not match ${deviceSoc}, but model .so/.dlc is present so device-local context generation can proceed`,
      );
      return;
    }
    fail(
      "QNN context target",
      `model target ${targets.join(", ")} does not match connected device ${deviceSoc}`,
    );
  }
}

function checkModelGenerationInputs() {
  if (!hasGeneratorInput()) {
    warn(
      "QNN context generation input",
      "no model .so or .dlc found under models\\sensevoice; current asset appears to be a prebuilt context binary",
    );
  } else {
    ok("QNN context generation input", "found .so or .dlc");
  }
}

function main() {
  console.log("============================================================");
  console.log("  QNN prereq check");
  console.log("============================================================");

  requireDir(qnnSdk, "QNN SDK");
  requireDir(path.join(root, "third_party", "sherpa-onnx-src"), "sherpa-onnx source");
  requireDir(path.join(root, "third_party", "android-ndk-r27c"), "Android NDK");
  requireDir(senseVoiceDir, "SenseVoice dir");
  const hasContext = optionalFile(path.join(senseVoiceDir, "model.bin"), "SenseVoice model.bin");
  const hasModelLib = optionalFile(path.join(senseVoiceDir, "libmodel.so"), "SenseVoice libmodel.so");
  if (!hasContext && !hasModelLib) {
    fail("SenseVoice QNN model asset", "need model.bin or libmodel.so");
  }
  requireFile(path.join(senseVoiceDir, "tokens.txt"), "SenseVoice tokens.txt");

  for (const lib of [
    "libQnnHtp.so",
    "libQnnHtpPrepare.so",
    "libQnnSystem.so",
  ]) {
    requireFile(path.join(qnnSdk, "lib", "aarch64-android", lib), lib);
  }

  const deviceSoc = readDeviceSoc();
  if (deviceSoc) readQnnSocId(deviceSoc);
  checkHtpArchLibraries(deviceSoc);
  checkModelTarget(deviceSoc);
  checkModelGenerationInputs();

  console.log("");
  if (failures > 0) {
    console.log(`QNN prereq check failed: ${failures} failure(s), ${warnings} warning(s).`);
    process.exit(1);
  }
  console.log(`QNN prereq check passed: ${warnings} warning(s).`);
}

if (require.main === module) {
  main();
}

module.exports = {
  inferModelTargets,
  inferHtpArch,
};
