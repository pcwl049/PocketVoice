const assert = require("assert");
const fs = require("fs");
const path = require("path");

const root = path.resolve(__dirname, "..");
const cmake = fs.readFileSync(
  path.join(root, "src/mobile/app/src/main/cpp/CMakeLists.txt"),
  "utf8",
);
const engine = fs.readFileSync(
  path.join(root, "src/mobile/app/src/main/cpp/stt_engine.cpp"),
  "utf8",
);

const target = cmake.match(/add_library\(stt_native SHARED\s*([\s\S]*?)\n\)/);
assert(target, "stt_native source list must exist");
const sourceList = target[1];

assert(
  !sourceList.includes("qwen3_qnn_backend.cpp"),
  "CPU builds must not compile the Qwen QNN backend",
);
assert(
  /if\s*\(STT_USE_QNN\)[\s\S]*target_sources\s*\(\s*stt_native\s+PRIVATE[\s\S]*qwen3_qnn_backend\.cpp/.test(cmake),
  "Qwen QNN sources must be added only when STT_USE_QNN is enabled",
);
assert(
  /#if STT_USE_QNN\s*#include "qwen3_qnn_backend\.h"\s*#include "qwen3_tokenizer\.h"\s*#endif/.test(engine),
  "CPU builds must not include Qwen QNN implementation headers",
);
assert(
  /#if STT_USE_QNN\s*if \(m_backendType == BackendType::Qwen3AsrQnn\)[\s\S]*?return result;\s*}\s*#endif/.test(engine),
  "CPU builds must exclude the Qwen QNN recognition branch",
);

console.log("cpu_android_build_config tests passed");
