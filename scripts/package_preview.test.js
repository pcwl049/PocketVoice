const assert = require("assert");
const fs = require("fs");
const path = require("path");

const root = path.resolve(__dirname, "..");
const script = fs.readFileSync(path.join(root, "scripts", "package_preview.bat"), "utf8");
const verify = fs.readFileSync(path.join(root, "scripts", "verify_release.bat"), "utf8");

assert(script.includes("PocketVoice-preview"), "package script should create a single preview folder");
assert(script.includes("PocketVoice-preview.zip"), "package script should create a zip archive");
assert(script.includes("PocketVoice.exe"), "package script should rename the PC executable for users");
assert(script.includes("adb.exe"), "package script should include adb.exe");
assert(script.includes("AdbWinApi.dll"), "package script should include AdbWinApi.dll");
assert(script.includes("AdbWinUsbApi.dll"), "package script should include AdbWinUsbApi.dll");
assert(script.includes("NOTICE.txt"), "package script should keep platform-tools notices when present");
assert(script.includes("PocketVoice-Android.apk"), "package script should include the Android APK when available");
assert(script.includes("Compress-Archive"), "package script should create the zip through PowerShell");
assert(verify.includes("package_preview.test.js"), "release static verification should include package script coverage");

console.log("package_preview tests passed");
