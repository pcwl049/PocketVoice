const assert = require("assert");
const fs = require("fs");
const path = require("path");
const {
  parseExpectedTsv,
  parseDryRunText,
  parsePcText,
  androidLogContainsBackend,
  serverStartAttempts,
  textMatchesExpectation,
} = require("./pc_wav_suite");

const rows = parseExpectedTsv([
  "file\tmode\texpected_contains",
  "test_audio/zh-short.wav\twav\t对我做了介绍",
  "test_audio/zh-short.wav\twav-vad\t对我做了介绍",
  "test_audio/zh-short.wav\tsimulate-wav-vad\t对我做了介绍",
].join("\n"));

assert.strictEqual(rows.length, 3);
assert.deepStrictEqual(rows[0], {
  file: "test_audio/zh-short.wav",
  mode: "wav",
  expected_contains: "对我做了介绍",
});

assert.strictEqual(
  parsePcText("[Timing] PC send-to-text: 244 ms\n[Text] 对我做了介绍那么我想说的是呢\nExit: 0"),
  "对我做了介绍那么我想说的是呢",
);
assert.strictEqual(
  parseDryRunText("[Text] 对我做了介绍那么我想说的是呢\n[ChatBox dry-run] 对我做了介绍那么我想说的是呢\nExit: 0"),
  "对我做了介绍那么我想说的是呢",
);

assert.strictEqual(
  textMatchesExpectation("对我做了介绍那么我想说的是呢", "对我做了介绍"),
  true,
);
assert.strictEqual(
  textMatchesExpectation("重点呢想谈三个问题", "对我做了介绍"),
  false,
);

assert.strictEqual(
  androidLogContainsBackend("I/STT_Native: Recognizer backend: sensevoice_qnn", "sensevoice_qnn"),
  true,
);
assert.strictEqual(
  androidLogContainsBackend("I/STT_Native: Recognizer backend: zipformer_ctc", "sensevoice_qnn"),
  false,
);

delete process.env.STT_SERVER_START_ATTEMPTS;
assert.strictEqual(serverStartAttempts(), 2);
process.env.STT_SERVER_START_ATTEMPTS = "3";
assert.strictEqual(serverStartAttempts(), 3);
process.env.STT_SERVER_START_ATTEMPTS = "0";
assert.strictEqual(serverStartAttempts(), 1);
delete process.env.STT_SERVER_START_ATTEMPTS;

assert.strictEqual(
  parseDryRunText("[ChatBox dry-run] 重点呢想谈三个问题。\n[ChatBox dry-run] 首先呢就是这一轮全球金融动荡的表现。\n"),
  "重点呢想谈三个问题。首先呢就是这一轮全球金融动荡的表现。",
);

const root = path.resolve(__dirname, "..");
const protocol = fs.readFileSync(path.join(root, "src/common/protocol.h"), "utf8");
const pcClientHeader = fs.readFileSync(path.join(root, "src/pc/network/client.h"), "utf8");
const pcClientSource = fs.readFileSync(path.join(root, "src/pc/network/client.cpp"), "utf8");
const pcMain = fs.readFileSync(path.join(root, "src/pc/main.cpp"), "utf8");
const configHeader = fs.readFileSync(path.join(root, "src/common/config.h"), "utf8");
const wasapiHeader = fs.readFileSync(path.join(root, "src/pc/audio/wasapi_capture.h"), "utf8");
const wasapiSource = fs.readFileSync(path.join(root, "src/pc/audio/wasapi_capture.cpp"), "utf8");
const statusServerSource = fs.readFileSync(path.join(root, "src/pc/status_http_server.cpp"), "utf8");
const fireredVadHeader = fs.existsSync(path.join(root, "src/pc/audio/firered_vad_ort.h"))
  ? fs.readFileSync(path.join(root, "src/pc/audio/firered_vad_ort.h"), "utf8")
  : "";
const fireredVadSource = fs.existsSync(path.join(root, "src/pc/audio/firered_vad_ort.cpp"))
  ? fs.readFileSync(path.join(root, "src/pc/audio/firered_vad_ort.cpp"), "utf8")
  : "";
const embeddedVadHeader = fs.existsSync(path.join(root, "src/pc/embedded_vad_model.h"))
  ? fs.readFileSync(path.join(root, "src/pc/embedded_vad_model.h"), "utf8")
  : "";
const embeddedVadSource = fs.existsSync(path.join(root, "src/pc/embedded_vad_model.cpp"))
  ? fs.readFileSync(path.join(root, "src/pc/embedded_vad_model.cpp"), "utf8")
  : "";
const pcResource = fs.existsSync(path.join(root, "src/pc/pc_resources.rc"))
  ? fs.readFileSync(path.join(root, "src/pc/pc_resources.rc"), "utf8")
  : "";
const buildPc = fs.readFileSync(path.join(root, "scripts/build_pc.bat"), "utf8");
assert(protocol.includes("FLAG_HAS_SEGMENT_ID"), "shared protocol should define the segment-id flag");
assert(
  pcClientHeader.includes("segment_id") &&
    pcClientHeader.includes("segmentId") &&
    pcClientSource.includes("setHasSegmentId(true)") &&
    pcClientSource.includes("FLAG_HAS_SEGMENT_ID") &&
    pcMain.includes("g_nextSegmentId"),
  "PC transport should send and parse segment ids",
);
assert(
  pcMain.includes("--list-audio-devices") &&
    pcMain.includes("--audio-device-id") &&
    wasapiHeader.includes("AudioInputDevice") &&
    wasapiHeader.includes("listInputDevices") &&
    wasapiSource.includes("EnumAudioEndpoints") &&
    wasapiSource.includes("GetId") &&
    pcMain.includes("/control/audio/input-device"),
  "PC should support listing and selecting WASAPI input devices",
);
assert(
  statusServerSource.includes("Content-Length:") &&
    statusServerSource.includes("body.size() < contentLength") &&
    statusServerSource.includes("m_controlFn(path, requestBody)"),
  "PC status server should pass full POST bodies to control handlers",
);
assert(
  embeddedVadHeader.includes("embeddedSileroVadPath") &&
    embeddedVadSource.includes("FindResource") &&
    embeddedVadSource.includes("STT_SILERO_VAD_ONNX") &&
    pcResource.includes("STT_SILERO_VAD_ONNX") &&
    pcResource.includes("models\\\\silero_vad.onnx") &&
    buildPc.includes("pc_resources.rc") &&
    buildPc.includes("rc.exe") &&
    pcMain.includes("embeddedSileroVadPath") &&
    !buildPc.includes("copy /Y \"%ROOT_DIR%\\models\\silero_vad.onnx\""),
  "PC build should embed the Silero VAD model instead of copying it as an external model file",
);
assert(
  configHeader.includes('std::string backend = "firered"') &&
    configHeader.includes('std::string model = "models/fireredvad"') &&
    configHeader.includes('parseInSectionString("vad", "backend"') &&
    pcMain.includes("resolveVadModelPath") &&
    pcMain.includes("g_config.vad.backend") &&
    fireredVadHeader.includes("FireRedVadOrt") &&
    fireredVadSource.includes("OrtGetApiBase") &&
    fireredVadSource.includes("fireredvad_stream_vad_with_cache.onnx") &&
    fireredVadSource.includes("cmvn.ark") &&
    buildPc.includes("firered_vad_ort.cpp") &&
    buildPc.includes("firered_frontend\\fft.cpp") &&
    buildPc.includes("build\\qnn-android\\headers"),
    buildPc.includes("models\\fireredvad") &&
    buildPc.includes("fireredvad_stream_vad_with_cache.onnx"),
  "PC VAD should default to the FireRedVAD ONNX backend while keeping Silero as fallback",
);
assert(
  buildPc.includes("PocketVoice - PC Build Script") &&
    !buildPc.includes("VRChat STT - PC Build Script") &&
    pcMain.includes("PocketVoice - Speech to ChatBox") &&
    !pcMain.includes("VRChat STT - Speech to ChatBox"),
  "PC user-facing console/build titles should use PocketVoice branding",
);

console.log("pc_wav_suite tests passed");
