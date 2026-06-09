const assert = require("assert");
const fs = require("fs");
const path = require("path");

const root = path.resolve(__dirname, "..");
const header = fs.readFileSync(
  path.join(root, "third_party/sherpa-onnx-src/sherpa-onnx/c-api/c-api.h"),
  "utf8",
);
const source = fs.readFileSync(
  path.join(root, "third_party/sherpa-onnx-src/sherpa-onnx/c-api/c-api.cc"),
  "utf8",
);
const cxxHeader = fs.readFileSync(
  path.join(root, "third_party/sherpa-onnx-src/sherpa-onnx/c-api/cxx-api.h"),
  "utf8",
);
const cxxSource = fs.readFileSync(
  path.join(root, "third_party/sherpa-onnx-src/sherpa-onnx/c-api/cxx-api.cc"),
  "utf8",
);
const senseVoiceQnnSource = fs.readFileSync(
  path.join(
    root,
    "third_party/sherpa-onnx-src/sherpa-onnx/csrc/qnn/offline-sense-voice-model-qnn.cc",
  ),
  "utf8",
);
const qnnModelSource = fs.readFileSync(
  path.join(root, "third_party/sherpa-onnx-src/sherpa-onnx/csrc/qnn/qnn-model.cc"),
  "utf8",
);
const qnnUtilsSource = fs.readFileSync(
  path.join(root, "third_party/sherpa-onnx-src/sherpa-onnx/csrc/qnn/utils.cc"),
  "utf8",
);

assert(
  header.includes("qnn_backend_lib") &&
    header.includes("qnn_context_binary") &&
    header.includes("qnn_system_lib"),
  "C API offline SenseVoice config should expose QNN backend, context, and system libraries",
);
assert(
  source.includes("sense_voice.qnn_config.backend_lib") &&
    source.includes("sense_voice.qnn_config.context_binary") &&
    source.includes("sense_voice.qnn_config.system_lib"),
  "C API should map QNN fields to the internal QNN config",
);
assert(
  cxxHeader.includes("qnn_backend_lib") &&
    cxxHeader.includes("qnn_context_binary") &&
    cxxHeader.includes("qnn_system_lib") &&
    cxxSource.includes("sense_voice.qnn_backend_lib") &&
    cxxSource.includes("sense_voice.qnn_context_binary") &&
    cxxSource.includes("sense_voice.qnn_system_lib"),
  "C++ wrapper should pass QNN fields through to the C API",
);
assert(
  senseVoiceQnnSource.includes("has_prompt_input_") &&
    senseVoiceQnnSource.includes("Expect one or two input tensors") &&
    senseVoiceQnnSource.includes("input_is_feature_major_") &&
    senseVoiceQnnSource.includes("transposed[d * expected_num_frames_ + t]") &&
    !senseVoiceQnnSource.includes("Expect two input tensors. Actual"),
  "SenseVoice QNN wrapper should accept fixed-prompt single-input model libraries",
);
assert(
  senseVoiceQnnSource.includes("SHERPA_ONNX_QNN_DUMP_TENSORS") &&
    senseVoiceQnnSource.includes("bool ShouldDumpTensors() const"),
  "SenseVoice QNN raw tensor dumps should be controlled by an explicit environment variable",
);
assert(
  !/if \(config_\.debug\)\s*\{[\s\S]{0,500}(WriteRaw|WriteTensorData)/.test(
    senseVoiceQnnSource,
  ) &&
    /if \(ShouldDumpTensors\(\)\)\s*\{[\s\S]{0,500}(WriteRaw|WriteTensorData)/.test(
      senseVoiceQnnSource,
    ),
  "SenseVoice QNN raw tensor writes should be gated by ShouldDumpTensors(), not debug logging",
);
assert(
  qnnModelSource.includes("QNN_DATATYPE_UFIXED_POINT_8") &&
    qnnUtilsSource.includes("QNN_DATATYPE_UFIXED_POINT_8") &&
    qnnUtilsSource.includes("uint8_t"),
  "QNN tensor helpers should support uint8 fixed-point tensors",
);

console.log("sherpa_qnn_c_api tests passed");
