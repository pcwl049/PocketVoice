const assert = require("assert");
const fs = require("fs");
const path = require("path");

const scriptPath = path.join(__dirname, "build_qnn_android.bat");
const script = fs.readFileSync(scriptPath, "utf8");

assert(
  script.includes("-DSHERPA_ONNX_ENABLE_WEBSOCKET=OFF"),
  "QNN build should disable websocket dependencies",
);
assert(
  script.includes("ONNXRUNTIME_DIRECT_DIR") &&
    script.includes("%ONNXRUNTIME_DIRECT_DIR%\\jni\\arm64-v8a\\libonnxruntime.so"),
  "QNN build should accept the existing direct onnxruntime extraction layout",
);
assert(
  script.includes("onnxruntime: OK - versioned cache") &&
    script.includes("onnxruntime: OK - build\\qnn-android\\jni\\arm64-v8a") &&
    script.includes("- source: %ONNXRUNTIME_DIRECT_DIR%\\jni\\arm64-v8a\\libonnxruntime.so") &&
    script.includes("- url: https://github.com/csukuangfj/onnxruntime-libs/releases/download/v%ONNXRUNTIME_VER%/onnxruntime-android-%ONNXRUNTIME_VER%.zip"),
  "QNN build should print explicit ONNX Runtime cache/download provenance",
);
assert(
  script.includes("--check-only"),
  "QNN build should support a check-only mode for prereq/path validation",
);
assert(
  !script.includes("echo   - onnxruntime: OK ("),
  "Batch echo inside parenthesized blocks should avoid raw parentheses",
);
assert(
  script.includes("ONNXRUNTIME_DIR_CMAKE") &&
    script.includes("SHERPA_ONNXRUNTIME_LIB_DIR=%ONNXRUNTIME_DIR_CMAKE%/jni/arm64-v8a") &&
    script.includes("SHERPA_ONNXRUNTIME_INCLUDE_DIR=%ONNXRUNTIME_DIR_CMAKE%/headers"),
  "ONNX Runtime env paths passed to CMake should use forward slashes",
);
assert(
  script.includes("INSTALL_PREFIX_CMAKE") &&
    script.includes("-DCMAKE_INSTALL_PREFIX=%INSTALL_PREFIX_CMAKE%"),
  "QNN build should install under the workspace build directory",
);
for (const version of ["V68", "V69", "V73", "V75", "V79", "V81"]) {
  assert(
    script.includes("for %%V in (V68 V69 V73 V75 V79 V81)") &&
      script.includes("libQnnHtp%%VStub.so") &&
      script.includes("libQnnHtp%%VCalculatorStub.so") &&
      script.includes("libQnnHtp%%VSkel.so"),
    `QNN build should copy HTP ${version} stub, calculator stub, and skel libraries`,
  );
}

const onnxRuntimeDirIndex = script.indexOf("set \"ONNXRUNTIME_DIR=");
const libEnvIndex = script.indexOf("set \"SHERPA_ONNXRUNTIME_LIB_DIR=");
const includeEnvIndex = script.indexOf("set \"SHERPA_ONNXRUNTIME_INCLUDE_DIR=");

assert(onnxRuntimeDirIndex >= 0, "ONNXRUNTIME_DIR must be defined");
assert(libEnvIndex >= 0, "SHERPA_ONNXRUNTIME_LIB_DIR must be defined");
assert(includeEnvIndex >= 0, "SHERPA_ONNXRUNTIME_INCLUDE_DIR must be defined");
assert(
  onnxRuntimeDirIndex < libEnvIndex && onnxRuntimeDirIndex < includeEnvIndex,
  "ONNXRUNTIME_DIR must be defined before SHERPA_ONNXRUNTIME_* env vars",
);

console.log("qnn_build_script tests passed");
