const assert = require("assert");
const fs = require("fs");
const path = require("path");

const root = path.resolve(__dirname, "..");
const source = fs.readFileSync(
  path.join(root, "third_party/sherpa-onnx-src/sherpa-onnx/csrc/qnn/qnn-model.cc"),
  "utf8",
);

for (const helper of [
  "TensorDataType(",
  "TensorQuantizeParams(",
  "TensorClientBuffer(",
  "TensorDimensions(",
  "TensorName(",
]) {
  assert(source.includes(helper), `qnn-model.cc should use ${helper}`);
}

assert(
  !source.includes("t->v1.clientBuf.data") &&
    !source.includes("t->v1.dataType") &&
    !source.includes("t->v1.quantizeParams"),
  "QNN model tensor access should not hard-code v1 fields for runtime tensors",
);

console.log("qnn_tensor_version_access tests passed");
