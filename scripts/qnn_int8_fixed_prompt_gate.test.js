const assert = require("assert");
const fs = require("fs");
const path = require("path");

const script = fs.readFileSync(
  path.join(__dirname, "test_qnn_int8_fixed_prompt_gate.bat"),
  "utf8",
);
const fixedPromptScript = fs.readFileSync(
  path.join(__dirname, "test_qnn_fixed_prompt_gate.bat"),
  "utf8",
);

assert(
  script.includes("test_qnn_fixed_prompt_gate.bat"),
  "legacy int8 gate should delegate to the neutral fixed-prompt gate",
);

for (const text of [
  "sensevoice-act16-fixed-prompt-expanded-preserve-layout-restrict",
  "STT_QNN_GRAPH_NAME=model",
  "STT_QNN_INPUT_LIST=x:=input0.float.raw",
  "STT_QNN_NATIVE_INPUT_TENSOR_NAMES=none",
  "STT_QNN_INPUT0_LAYOUT=onnx",
  "Optional: set STT_QNN_EXPECT_CONTAINS before running this script.",
  "qnn_net_run_reference_gate.js",
]) {
  assert(fixedPromptScript.includes(text), `fixed-prompt gate should include ${text}`);
}

console.log("qnn_int8_fixed_prompt_gate tests passed");
