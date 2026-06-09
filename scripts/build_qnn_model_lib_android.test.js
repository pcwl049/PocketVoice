const assert = require("assert");
const fs = require("fs");
const path = require("path");

const script = fs.readFileSync(
  path.join(__dirname, "build_qnn_model_lib_android.ps1"),
  "utf8",
);

for (const text of [
  "param(",
  "[string]$Variant",
  "build\\qnn-convert\\$Variant",
  "build\\qnn-model-lib-android\\$Variant",
  "share\\QNN\\converter\\jni",
  "Copy-Item (Join-Path $qnnJni '*') $jni -Recurse -Force",
  "QnnModel.cpp",
  "QnnWrapperUtils.cpp",
  "linux\\QnnModelPal.cpp",
  "llvm-objcopy.exe",
  "aarch64-linux-android21-clang++.cmd",
  "-Wl,-z,max-page-size=16384",
  "libmodel.so",
]) {
  assert(script.includes(text), `Android model lib script should include ${text}`);
}

console.log("build_qnn_model_lib_android tests passed");
