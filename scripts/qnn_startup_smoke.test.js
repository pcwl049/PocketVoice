const assert = require("assert");
const fs = require("fs");
const path = require("path");

const root = path.resolve(__dirname, "..");
const smokePath = path.join(root, "scripts", "test_qnn_startup_smoke.bat");
assert(fs.existsSync(smokePath), "QNN startup smoke script should exist");

const smoke = fs.readFileSync(smokePath, "utf8");
assert(smoke.includes("am force-stop com.stt.mobile"), "smoke should restart the app once");
assert(smoke.includes("am start -n com.stt.mobile/.MainActivity"), "smoke should launch MainActivity");
assert(smoke.includes("--ez autoStart true"), "smoke should use MainActivity autoStart instead of WebView button clicks");
assert(smoke.includes("adb forward tcp:27000 tcp:27000"), "smoke should set up USB port forwarding");
assert(smoke.includes("Server listening on port 27000"), "smoke should wait for the native server readiness log");
assert(smoke.includes("Recognizer backend: sensevoice_qnn"), "smoke should verify QNN backend selection");
assert(smoke.includes("STT_QNN_STARTUP_TIMEOUT_MS"), "smoke should support a bounded startup timeout");
assert(!smoke.includes("timeout /t"), "smoke should not use timeout /t because it fails under redirected stdin");
assert(smoke.includes("Start-Sleep -Seconds 1"), "smoke should sleep with PowerShell in non-interactive runs");
assert(!smoke.includes("input keyevent 61"), "smoke should not rely on keyboard focus to press Start");
assert(!smoke.includes("input keyevent 66"), "smoke should not rely on keyboard focus to press Start");

const verify = fs.readFileSync(path.join(root, "scripts", "verify_release.bat"), "utf8");
assert(
  verify.includes("test_qnn_startup_smoke.bat") &&
    verify.indexOf("test_qnn_startup_smoke.bat") < verify.indexOf("test_qnn_pc_wav_suite.bat"),
  "release verification should run QNN startup smoke before the full QNN WAV suite",
);

console.log("qnn_startup_smoke tests passed");
