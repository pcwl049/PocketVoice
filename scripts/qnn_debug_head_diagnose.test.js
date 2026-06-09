const assert = require("assert");
const fs = require("fs");
const path = require("path");

const script = fs.readFileSync(
  path.join(__dirname, "qnn_debug_head_diagnose.js"),
  "utf8",
);

for (const tensor of [
  "_encoder_tp_norm_LayerNormalization_output_0_native.raw",
  "_ctc_lo_MatMul_pre_reshape_native.raw",
  "logits_fc_native.raw",
  "logits_native.raw",
]) {
  assert(script.includes(tensor), `diagnostic script should inspect ${tensor}`);
}

assert(
  script.includes("changedInsideLoad") &&
    script.includes("maxLoadEnd") &&
    script.includes("differingOffsets"),
  "diagnostic script should audit whether ELF repair changed mapped runtime bytes",
);

assert(
  script.includes("--strict") && script.includes("process.exit(1)"),
  "diagnostic script should support strict gating for automated acceptance",
);

assert(
  script.includes("final ctc_lo output is all zero while its input is non-zero"),
  "diagnostic script should report the current final-head zero-output blocker",
);

console.log("qnn_debug_head_diagnose tests passed");

