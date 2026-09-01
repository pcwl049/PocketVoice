const assert = require("assert");
const fs = require("fs");
const path = require("path");

// The sherpa QNN path (SenseVoice QNN / Paraformer QNN) writes an HTP backend
// extensions config at init time. soc_id/dsp_arch used to be hardcoded to
// 85/v79 (SM8750 arch), which cannot initialize on devices whose HTP arch
// differs (e.g. SM8735 = soc 85 but v73). The writer must keep the defaults
// but allow per-model overrides via files or env, mirroring qnn_vtcm_mb.txt.
const engineSource = fs.readFileSync(
  path.join(__dirname, "..", "src", "mobile", "app", "src", "main", "cpp", "stt_engine.cpp"),
  "utf8",
);

assert(
  engineSource.includes("qnn_soc_id.txt") && engineSource.includes("STT_QNN_SOC_ID"),
  "HTP config should accept soc_id overrides from qnn_soc_id.txt or STT_QNN_SOC_ID",
);
assert(
  engineSource.includes("qnn_dsp_arch.txt") && engineSource.includes("STT_QNN_DSP_ARCH"),
  "HTP config should accept dsp_arch overrides from qnn_dsp_arch.txt or STT_QNN_DSP_ARCH",
);
assert(
  !/"soc_id": \d+/.test(engineSource) && !/"dsp_arch": "\w+"/.test(engineSource),
  "HTP config writer must not hardcode soc_id/dsp_arch literals",
);
assert(
  /return 85;/.test(engineSource) && /return "v79";/.test(engineSource),
  "Default soc_id=85 and dsp_arch=v79 must be preserved for backward compatibility",
);
assert(
  engineSource.includes("readQnnSocId(modelDir)") &&
    engineSource.includes("readQnnDspArch(modelDir)"),
  "writeQnnHtpConfig should consume the override helpers",
);

console.log("qnn_htp_config_override tests passed");
