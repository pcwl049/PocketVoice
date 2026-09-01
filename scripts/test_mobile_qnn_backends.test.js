const assert = require("assert");
const fs = require("fs");
const path = require("path");

// The device-test harness must encode the HyperOS/QNN pitfalls from
// docs/DEV_PITFALLS.md (D1-D6, S3) so a single run yields a trustworthy
// SenseVoice QNN / Paraformer QNN acceptance.
const script = fs.readFileSync(
  path.join(__dirname, "test_mobile_qnn_backends.sh"),
  "utf8",
);

assert(script.includes("svc power stayon usb"), "must keep the screen on (D1)");
assert(script.includes("restorecon"), "prefs write must restorecon (D5)");
assert(script.includes("chown"), "prefs write must chown to app uid (D5)");
assert(script.includes("am force-stop"), "policy change must restart the app (D4)");
assert(script.includes("am start -n"), "must start MainActivity first to load native libs (D6)");
assert(script.includes("PORT=27000") && script.includes("forward \"tcp:$PORT\""),
  "must forward to the Android service port (27000)");
assert(script.includes("Initialized OK"), "must wait for engine init, not just the port");
assert(script.includes("warm-up connection"), "must warm up the forward (D3)");
assert(script.includes("REQUEST_INTERVAL"), "must throttle requests (D3, >=2s)");
assert(script.includes("qnn_dsp_arch.txt") && script.includes("qnn_soc_id.txt"),
  "must deploy per-model soc_id/dsp_arch overrides for the HTP config");
assert(script.includes("sensevoice_qnn") && script.includes("paraformer_qnn"),
  "must verify both QNN backends by their stt_engine names");
assert(script.includes("bench_refs.txt"), "must compare against the bench reference transcripts");
assert(script.includes("get_current_policy") && script.includes("SKIP_RESTORE"),
  "must restore the original policy after testing");
assert(script.includes("SM8735") && script.includes("v73"),
  "must map SM8735 to HTP arch v73");

console.log("test_mobile_qnn_backends tests passed");
