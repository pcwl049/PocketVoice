const assert = require("assert");
const fs = require("fs");
const path = require("path");

const script = fs.readFileSync(
  path.join(__dirname, "convert_sensevoice_qnn_variant.bat"),
  "utf8",
);

for (const text of [
  "int8-preserve-layout-bias32",
  "act16-preserve-layout-restrict",
  "int8-preserve-layout-per-row-bias32",
  "sensevoice-int8-fixed-prompt-expanded-preserve-layout-bias32",
  "sensevoice-act16-fixed-prompt-expanded-preserve-layout-restrict",
  "sensevoice-int8-fixed-prompt-expanded-preserve-layout-per-row-bias32",
  "--preserve_io layout logits",
  "--bias_bitwidth 32",
  "--act_bitwidth 8",
  "--act_bitwidth 16",
  "--weights_bitwidth 8",
  "--restrict_quantization_steps \"-0x8000 0x7F7F\"",
  "--use_per_row_quantization",
  "--input_list \"%INPUT_LIST%\"",
  "--out_node logits",
  "PYTHONPATH=%QAIRT_ROOT%\\lib\\python",
  "qnn-onnx-converter",
]) {
  assert(script.includes(text), `converter variant script should include ${text}`);
}

console.log("convert_sensevoice_qnn_variant tests passed");
