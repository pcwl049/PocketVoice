const assert = require("assert");
const fs = require("fs");
const path = require("path");

const script = fs.readFileSync(
  path.join(__dirname, "sensevoice_onnx_reference_gate.py"),
  "utf8",
);

for (const text of [
  "onnxruntime",
  "decode_ctc",
  "expected-contains",
  "CPUExecutionProvider",
  "features.reshape(1, args.frames, args.feature_dim)",
]) {
  assert(script.includes(text), `ONNX reference gate should include ${text}`);
}

console.log("sensevoice_onnx_reference_gate tests passed");
