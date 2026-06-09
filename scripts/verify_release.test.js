const assert = require("assert");
const fs = require("fs");
const path = require("path");

const script = fs.readFileSync(
  path.join(__dirname, "verify_release.bat"),
  "utf8",
);

assert(
  script.includes("qnn_build_script.test.js") &&
    script.includes("sherpa_qnn_c_api.test.js") &&
    script.includes("mobile_qnn_integration.test.js"),
  "release verification should run the static regression tests",
);
assert(
  script.includes("test_pc_wav_suite.bat"),
  "release verification should run the CPU fallback WAV suite",
);
assert(
  script.includes("test_qnn_pc_wav_suite.bat"),
  "release verification should run the QNN Android WAV suite",
);
assert(
  script.includes("test_qnn_startup_smoke.bat") &&
    script.indexOf("test_qnn_startup_smoke.bat") < script.indexOf("test_qnn_pc_wav_suite.bat"),
  "release verification should run QNN startup smoke before the full QNN WAV suite",
);
assert(
  script.includes("--static-only") && script.includes("--no-qnn"),
  "release verification should provide fast and no-device modes",
);

console.log("verify_release tests passed");
