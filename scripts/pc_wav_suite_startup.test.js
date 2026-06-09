const assert = require("assert");
const fs = require("fs");
const path = require("path");

const source = fs.readFileSync(path.join(__dirname, "pc_wav_suite.js"), "utf8");
const runCaseMatch = source.match(/function runCase\([\s\S]*?\n}\n\nfunction main/);
assert(runCaseMatch, "runCase function should be present before main");

const runCaseSource = runCaseMatch[0];
assert(
  !runCaseSource.includes("startAndroidServer("),
  "runCase should not restart the Android app for every WAV case",
);

assert(
  source.includes("startSuiteAndroidServer(adb, resultFile)"),
  "main should start the Android app once before running the suite",
);

console.log("pc_wav_suite_startup tests passed");
