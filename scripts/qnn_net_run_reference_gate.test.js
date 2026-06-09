const assert = require("assert");
const fs = require("fs");
const path = require("path");

const script = fs.readFileSync(
  path.join(__dirname, "qnn_net_run_reference_gate.js"),
  "utf8",
);

for (const text of [
  "STT_QNN_NET_RUN_DIR",
  "inferHtpArch",
  "QNN_SOC_MODEL_",
  "libQnnHtp${htpArch}Stub.so",
  "libQnnHtp${htpArch}CalculatorStub.so",
  "libQnnHtp${htpArch}Skel.so",
  "[\"V68\", \"V69\", \"V73\", \"V75\", \"V79\", \"V81\"]",
  "libQnnHtpNetRunExtensions.so",
  "libc++_shared.so",
  "htp_backend_extensions.json",
  "htp_config.json",
  "qnn-net-run",
  "STT_QNN_REFERENCE_MODEL",
  "STT_QNN_BACKEND",
  "libQnnCpu.so",
  "STT_QNN_GRAPH_NAME",
  "STT_QNN_INPUT_LIST",
  "STT_QNN_INPUT0",
  "STT_QNN_INPUT1",
  "STT_QNN_INPUT0_LAYOUT",
  "prepareInput0",
  "onnx",
  "feature_major",
  "inputFiles",
  "backend",
  "backend === \"htp\" ? \"--config_file ./htp_backend_extensions.json\" : \"\"",
  "STT_QNN_NATIVE_INPUT_TENSOR_NAMES",
  "nativeInputTensorNamesArg",
  "\"none\"",
  "modelOverride",
  "model_10_seconds_quantized",
  "${graphName}:prompt",
  "logits.raw",
  "floatLogitsStats",
  "greedy",
  "greedyByLayout",
  "frame_major",
  "vocab_major",
  "decodeGreedy",
  "STT_QNN_EXPECT_CONTAINS",
  "decoded",
  "--report-only",
]) {
  assert(script.includes(text), `reference gate should include ${text}`);
}

assert(
  script.includes("qnn-net-run reference gate failed: frame-major float logits greedy output is all blank"),
  "reference gate should fail strict mode when frame-major float logits decode to all blanks",
);

console.log("qnn_net_run_reference_gate tests passed");
