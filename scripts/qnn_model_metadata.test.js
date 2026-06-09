const assert = require("assert");
const fs = require("fs");
const path = require("path");

const { summarize } = require("./qnn_model_metadata");

const originalPath = path.join(
  __dirname,
  "..",
  "build",
  "qnn-convert",
  "sensevoice-int8-fixed-prompt-expanded",
  "model_net.json",
);
const preserveLayoutPath = path.join(
  __dirname,
  "..",
  "build",
  "qnn-convert",
  "sensevoice-int8-fixed-prompt-expanded-preserve-layout",
  "model_net.json",
);

if (fs.existsSync(originalPath)) {
  const original = summarize(originalPath);
  assert.deepStrictEqual(original.io.x.dims, [1, 560, 167]);
  assert.deepStrictEqual(original.io.logits.dims, [1, 25055, 171]);
  assert.strictEqual(original.converter.bias_bitwidth, "32");
  assert(original.ctcTensors.some((tensor) => tensor.name === "logits_fc"));
}

if (fs.existsSync(preserveLayoutPath)) {
  const preserveLayout = summarize(preserveLayoutPath);
  assert.deepStrictEqual(preserveLayout.io.x.dims, [1, 560, 167]);
  assert.deepStrictEqual(preserveLayout.io.logits.dims, [1, 171, 25055]);
  assert.strictEqual(preserveLayout.converter.preserve_io, "[['layout', 'logits']]");
}

console.log("qnn_model_metadata tests passed");
