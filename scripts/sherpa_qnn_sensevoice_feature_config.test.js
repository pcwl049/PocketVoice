const assert = require("assert");
const fs = require("fs");
const path = require("path");

const root = path.resolve(__dirname, "..");
const tpl = fs.readFileSync(
  path.join(
    root,
    "third_party/sherpa-onnx-src/sherpa-onnx/csrc/offline-recognizer-sense-voice-tpl-impl.h",
  ),
  "utf8",
);
const reference = fs.readFileSync(
  path.join(
    root,
    "third_party/sherpa-onnx-src/scripts/sense-voice/qnn/generate_test_data.py",
  ),
  "utf8",
);

assert(
  reference.includes("opts.frame_opts.snip_edges = False"),
  "QNN SenseVoice reference test-data generator should document snip_edges=False",
);
assert(
  tpl.includes("config_.feat_config.snip_edges = false;"),
  "QNN SenseVoice runtime feature extraction should match the QNN reference generator snip_edges=False",
);

console.log("sherpa_qnn_sensevoice_feature_config tests passed");
