const assert = require("assert");
const fs = require("fs");
const path = require("path");

const root = path.resolve(__dirname, "..");
const scriptPath = path.join(root, "scripts", "check_qnn_prereqs.js");
const batPath = path.join(root, "scripts", "check_qnn_prereqs.bat");
const qnnSuitePath = path.join(root, "scripts", "test_qnn_pc_wav_suite.bat");
const qnnReferenceGatePath = path.join(root, "scripts", "test_qnn_reference_gate.bat");

const script = fs.readFileSync(scriptPath, "utf8");
const bat = fs.readFileSync(batPath, "utf8");
const qnnSuite = fs.readFileSync(qnnSuitePath, "utf8");
const qnnReferenceGate = fs.readFileSync(qnnReferenceGatePath, "utf8");

assert(
  script.includes("ro.soc.model") &&
    script.includes("QnnTypes.h") &&
    script.includes("QNN_SOC_MODEL_"),
  "QNN prereq checker should read the connected device SoC and verify the SDK enum",
);
assert(
  script.includes("STT_QNN_MODEL_TARGET") &&
    script.includes("model target") &&
    script.includes("does not match connected device"),
  "QNN prereq checker should fail when the model target does not match the connected device",
);
assert(
  script.includes("SenseVoice libmodel.so") &&
    script.includes("need model.bin or libmodel.so"),
  "QNN prereq checker should accept libmodel.so as a first-run context generation input",
);
assert(
  script.includes("[\"SM8735\", \"V79\"]") &&
    script.includes("libQnnHtp${arch}Stub.so") &&
    script.includes("libQnnHtp${arch}CalculatorStub.so") &&
    script.includes("libQnnHtp${arch}Skel.so"),
  "QNN prereq checker should map SM8735 to HTP V79 and check matching HTP libraries",
);
assert(
  script.includes(".dlc") &&
    script.includes(".so") &&
    script.includes("prebuilt context binary"),
  "QNN prereq checker should warn when only a context binary is present",
);
assert(
  bat.includes("check_qnn_prereqs.js"),
  "Batch prereq entrypoint should delegate to the Node checker",
);
assert(
  qnnSuite.indexOf("check_qnn_prereqs.bat") < qnnSuite.indexOf("build_qnn_android.bat"),
  "QNN regression suite should run prereq checks before expensive builds",
);
assert(
  qnnReferenceGate.includes("qnn_net_run_reference_gate.js"),
  "QNN reference gate batch entrypoint should run the qnn-net-run reference gate",
);

const { inferHtpArch, inferModelTargets } = require(scriptPath);
const targets = inferModelTargets();
assert(
  targets.includes("SM8550"),
  "Current SenseVoice asset directory should be recognized as an SM8550 QNN target",
);
assert.strictEqual(
  inferHtpArch("SM8735"),
  "V79",
  "inferHtpArch should map SM8735 to V79",
);
assert.strictEqual(
  inferHtpArch("SM8550"),
  "V73",
  "inferHtpArch should map SM8550 to V73",
);

console.log("check_qnn_prereqs tests passed");
