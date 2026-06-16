# Qwen3 QNN Next Tasks For AI

Last updated: 2026-06-16

This is the active handoff for the next AI worker. It replaces older appended task lists.

## Read First

```text
docs/architecture/QWEN3_QNN_DECODER_EXPERIMENT_LOG.md
docs/architecture/QWEN3_QNN_ANDROID_INTEGRATION_SPEC.md
docs/architecture/QWEN3_QNN_DIAGNOSIS_REPORT.md
docs/superpowers/specs/2026-06-15-gate-b3-tiny-custom-io-probe-design.md
docs/superpowers/plans/2026-06-15-gate-b3-tiny-custom-io-probe.md
docs/superpowers/specs/2026-06-15-gate-b4-bridge-probe-design.md
docs/superpowers/specs/2026-06-15-gate-c-raw-buffer-probe-design.md
docs/superpowers/plans/2026-06-15-gate-c-raw-buffer-probe.md
docs/superpowers/specs/2026-06-15-gate-c2-apk-cache-buffer-probe-design.md
docs/superpowers/plans/2026-06-15-gate-c2-apk-cache-buffer-probe.md
third_party/sherpa-onnx-src/sherpa-onnx/csrc/offline-recognizer-qwen3-asr-impl.cc
```

## Current Status

```text
Gate 1: QNN init smoke                 PASSED
Gate 2: W=4 in-app smoke gate          PASSED
Gate 3: audio_features quant fix       PASSED
Gate 4: sherpa generation cleanup      PASSED
Gate 5: real-audio E2E with W=4        FAILED (superseded: Paraformer QNN HTP bypasses this)
Gate 6: W=128 decoder build + init     PASSED
Gate 7: real-audio E2E with W=128      FAILED (superseded: Paraformer QNN HTP bypasses this)
Gate 8: KV cache quantization           CONFIRMED ROOT CAUSE (Step 18 diagnosis)
Gate 9: KV cache quant override fix     PASSED (model_net.json verified, cache_key/value is_overridden=True)
Gate 10: real-audio E2E with KV fix      FAILED (garbage but different from W=4, KV data now non-zero)
Gate 11: position encoding fix (0-indexed)  PASSED (position_scalar starts at 0, past KV correctly masked)
Gate 12: real-audio E2E with position fix   FAILED (repetition loop: argmax=124463 repeated 106x/207 steps)
Gate 13: prewindow decoder (Slice removed)  PASSED (W=128, Slice nodes eliminated, smoke test OK)
Gate 14: KV influence probe (prewindow)     FAILED (HTP overrides cache_key scale to 1.53e-9, KV data invisible)
Gate 15: non-zero calibration data fix       FAILED (HTP graphFinalize ignores quant_overrides regardless)
Gate A: runtime encoding audit               FAILED (Path1+Path2 both confirm scale=1.53e-9, HTP overrides at finalize)
Gate B: --preserve_io datatype float32       FAILED (HTP overrides dtype from FLOAT32 to UFIXED16+scale=1.53e-9)
Gate B3: custom_io QuantParam                INCONCLUSIVE (tiny float32 model keeps FLOAT_32 at runtime, but real decoder failed in Route 2)
Gate B4: bridge probe for HTP override trigger INCONCLUSIVE (no small model reproduces the UFIXED16 1.53e-9 override; --preserve_io blocked by Convert node rejection)
Gate C: raw-buffer connectivity probe          INCONCLUSIVE (qnn-net-run cannot finalize the 912 MB real decoder on HTP, error 1002)
Gate C2: APK-embedded cache buffer probe       FAILED (HTP does NOT read APP_WRITE cache buffers; zero/maxpattern/realkv outputs identical)
Gate D1: CPU decoder fallback probe              PRODUCT-FAILED (3313ms decode, correct output, but user requires <=1000ms latency)
Gate D3: Paraformer QNN HTP device test       PASSED (73ms decode, correct output)
Gate E: ORT + XNNPACK Paraformer offline       PASSED (342ms decode, correct output, XNNPACK EP active)
```

Current conclusion (updated Step 30 / Gate E):

```text
Gate E proved ORT + XNNPACK works as the preferred non-QNN fallback backend.
ParaformerXnnpack achieves 342ms decode for 5.61s audio (0.061x RTF) with
correct Chinese output. XNNPACK execution provider is active (not falling back
to CPU). This is well within the <=1000ms product target.

Backend priority on Android:
  Primary:      ParaformerQnn (73ms, QNN HTP)
  Fallback 1:   ParaformerXnnpack (342ms, ORT + XNNPACK)
  Fallback 2:   Qwen3AsrCpu (3313ms, correctness baseline)

NEXT: Measure peak RSS for ParaformerXnnpack, validate with more diverse audio
samples, and run the Qwen3-ASR LiteRT + Qualcomm Delegate compatibility check.
```

## Confirmed Evidence

### QNN Runtime Works

Android can initialize and run:

```text
backend = qwen3_asr_qnn
QNN interface obtained: backend=6, provider=HTP_QTI_AISW
QnnBackend_create
QnnDevice_create
QnnContext_create
QnnModel_composeGraphs
graph finalize
TCP server start on port 27000
```

Do not spend time on QNN init unless it regresses.

### W=4 Smoke Gate Is Now Real

`Qwen3QnnBackend::runDecoderSmokeTest()` was fixed in experiment log Step 16.

Current cumulative-cache W=4 reference:

```text
Step 1: argmax=0,      logits_sum=-290501.69, cache_nonzero=57272
Step 2: argmax=660,    logits_sum=-546629.12, cache_nonzero=114560
Step 3: argmax=128629, logits_sum=-355988.12, cache_nonzero=171859
```

Rules now enforced:

```text
argmax must match exactly
logits_sum uses tolerance
cache_nonzero must grow
smoke returns false on mismatch
```

Do not restore the old per-step-reset smoke behavior.

### audio_features Quantization Is Fixed

Device logs confirmed:

```text
audio_features scale ~= 3.0518e-06
attention_bias scale ~= 1.5259e-01
```

Smoke test still passes after the quant fix. Do not reconvert the decoder for this same issue again.

### Real Audio Reaches The Decoder

Real-device logs confirmed nonzero encoder output reaches decoder input:

```text
encoder output:
  count=79872
  min=-0.143290
  max=0.107388
  sum=54.47
  nonzero=66560

decoder input:
  count=66560
  min=-0.143290
  max=0.107388
  sum=54.47
  nonzero=66560
```

### Generation Cleanup Works Mechanically

Android generation path now includes:

```text
special-token stops:
  <|im_end|>
  <|endoftext|>
  <|audio_start|>
  <|audio_end|>
  <|audio_pad|>
  <|im_start|>

full-token-list decode
UTF-8 replacement char cleanup
leading "language ... <asr_text>" cleanup
max_total_len tracking
first-token EOS retry
```

Real-audio E2E still returned garbage after this. Generation cleanup should stay, but more tuning here is not the next priority.

## Current Failure

Real-audio E2E with W=128:

```text
Audio: models/zipformer-ctc/test_wavs/0.wav
Prompt length: 80 tokens
Backend: qwen3_asr_qnn (HTP)
Result: garbage text, but no longer byte-identical to the old W=4/W=128 output
```

Key generation evidence:

```text
Step 19 after KV quant override fix:
- prompt structure is correct
- cache_nonzero grows from 0 to about 6.9M across 122 steps
- generated text remains incoherent
- token sequence differs from the old W=4/W=128-audio-fix result
```

Gate 9 fixed the confirmed KV quantization blocker:

```text
cache_key_0..27:   is_overridden=True, range=[-512, 512]
cache_value_0..27: is_overridden=True, range=[-1, 1]
```

Do not spend more time on quant_overrides unless this verification regresses.

## Hard Boundaries

Do not modify:

```text
README.md
src/common/protocol.h
src/pc/*
archive/legacy/*
UI
release files
QNN init path
audio_features quantization fix
W=4 smoke gate unless it regresses
```

Allowed focus:

```text
Qwen3 decoder export/conversion strategy
Qwen3 fixed-window size experiment
Android decoder artifact selection
scripts needed for conversion/build/test
docs/architecture evidence logs
src/mobile/app/src/main/cpp/qwen3_qnn_backend.cpp
src/mobile/app/src/main/cpp/qwen3_qnn_backend.h
```

## Next Task: Peak RSS Measurement, Diverse Audio Validation, and LiteRT Compatibility Check

Gate E is PASSED. ParaformerXnnpack produces correct output at 342ms decode
latency (0.061x RTF), well within the <=1000ms product target.

```text
Immediate next steps:
1. Measure peak RSS for ParaformerXnnpack and compare with ParaformerQnn
2. Validate with more diverse audio samples (longer audio, noisy audio, etc.)
3. Run a short Qwen3-ASR -> LiteRT + Qualcomm Delegate compatibility check before
   starting any implementation work on that route
```

Do not continue Qwen3 QNN HTP decoder override research unless a new
primary-source mechanism appears. The Paraformer non-autoregressive
architecture avoids the KV cache failure mode entirely.

### Fallback Backend Policy

Use this priority for Android offline backends:

```text
Primary:      ParaformerQnn
Fallback 1:   ParaformerXnnpack (ORT + XNNPACK, 342ms)
Fallback 2:   Qwen3AsrCpu correctness baseline (3313ms)
Legacy:       Zipformer/Paraformer CPU streaming only when explicitly selected for comparison
Research:     Qwen3-ASR LiteRT + Qualcomm Delegate compatibility check only
```

`ParaformerXnnpack` replaces generic "ORT + XNNPACK" language as the concrete
non-QNN runtime fallback. It is intended for compatibility and debugging, not as
the main quality route unless measured accuracy and latency justify it.

Do not add LiteRT, MNN, NNAPI, and XNNPACK at the same time. Each added backend
must have a named model target, expected artifact format, and a device test plan.

### Qwen3-ASR LiteRT + Qualcomm Delegate Compatibility Check

This is a stop/go check, not an implementation task. The goal is to avoid another
multi-day compatibility investigation.

Stop immediately if any required check fails:

```text
1. Artifact check
   Confirm there is a real Qwen3-ASR LiteRT/TFLite artifact or a documented
   conversion path for the exact ASR model variant. Do not start from a vague
   "Qwen-like model" assumption.

2. Signature check
   Confirm the model exposes a LiteRT-LM-compatible prefill/decode structure or
   an equivalent fixed API for encoder output, decoder input, position, and KV
   cache state.

3. KV cache check
   Confirm KV cache is managed by LiteRT-LM/stateful inference or a documented
   Qualcomm Delegate path. If KV is only represented as arbitrary raw tensors
   without state semantics, treat the route as high risk and stop.

4. Delegate support check
   Run a minimal Qualcomm Delegate compile/init smoke on the target phone. The
   check must prove the model is accepted by the delegate, not only by CPU
   LiteRT.

5. Token smoke check
   If init succeeds, run one tiny prefill/decode step and compare token/logit
   behavior against a CPU reference. Do not proceed to APK integration until this
   is numerically plausible.
```

Expected result format:

```text
Qwen3 LiteRT Qualcomm Check: PASS / FAIL / BLOCKED
Artifact: <path or missing>
Delegate init: <ok/fail + exact log>
KV handling: <LiteRT-LM stateful / raw tensors / unknown>
Token smoke: <match / mismatch / not run>
Decision: <continue / stop>
```

### Already Completed

```text
Gate A: runtime encoding audit
Gate B Route 2: full decoder --preserve_io datatype float32
Gate B3: tiny custom_io / float32 probe
Gate B4: bridge probe for HTP override trigger (INCONCLUSIVE)
Gate C: qnn-net-run raw-buffer probe (INCONCLUSIVE, graphFinalize 1002)
Gate C2: APK cache-buffer probe (FAILED, no observable KV influence)
Gate D1: CPU decoder fallback probe (CORRECT_BUT_TOO_SLOW, 3313ms decode, target <=1000ms)
Gate E: ORT + XNNPACK Paraformer offline (PASSED, 342ms decode, XNNPACK EP active)
```

Do not repeat these unchanged.

### Gate E: COMPLETED (PASSED)

Evidence from Step 30:

```text
Backend: paraformer_xnnpack
Init: ~2.6s
Model: model.int8.onnx (232 MB) + tokens.txt
XNNPACK EP: active (not falling back to CPU)
Decode: 342ms for 5.61s audio (0.061x RTF)
Output: "对我做了介绍啊那么我想说的是呢大家如果对我的研究感兴趣呢你" (correct)
```

Interpretation:

```text
ORT + XNNPACK is a viable non-QNN fallback backend for Paraformer offline.
It delivers 342ms decode latency, well within the <=1000ms product target,
though 4.7x slower than ParaformerQnn on HTP (73ms). The XNNPACK execution
provider is confirmed active at runtime.
```

### Gate C2: COMPLETED (FAILED)

Evidence from Step 27:

```text
Run A zero:       logits_sum=-290501.69, argmax=0
Run B maxpattern: logits_sum=-290501.69, argmax=0
Run C realkv-like: logits_sum=-290501.69, argmax=0
key_delta_0/value_delta_0 metrics also identical.
```

Interpretation:

```text
The current-token path still works, but past-KV cache inputs have no observable
effect. This blocks correct multi-token autoregressive decoding on HTP.
```

### Gate D1: COMPLETED (CORRECT BUT TOO SLOW)

Evidence from Step 28:

```text
Backend: qwen3_asr_cpu
Init: 6,745 ms, RSS 1,457,320 KB
Decode: 3,313 ms for 5,611 ms audio (0.59x RTF)
Peak RSS: 1,661,192 KB (~1.6 GB)
Output: "对我做了介绍啊。那么我想说的是呢，大家如果对我的研究感兴趣呢。" (correct)
```

Interpretation:

```text
CPU decoder is viable as a correctness baseline and fallback, but it misses the
product latency target. User requires <=1000ms per recognition segment; measured
decode time is 3313ms.
```

### Historical Task: Gate D3 Low-Latency Model / Runtime Selection

Gate D3 must find a path that can plausibly hit <=1000ms per segment while
keeping Chinese ASR quality acceptable. Do not spend more time making Qwen3AsrCpu
the default as the main product path.

```text
Latency target:
  <=1000ms recognition latency per segment on the target phone.

Resolved candidates:
1. Paraformer QNN HTP passed and is the current production path
2. ORT + XNNPACK is the preferred non-QNN fallback to add next
3. Qwen3-ASR LiteRT + Qualcomm Delegate is research-only until the minimal
   compatibility check passes
4. Cloud/API mode remains optional high-quality fallback, not offline default
```

Keep Qwen3AsrCpu as a correctness baseline for comparing output quality.

### Gate D Candidate Routes

Evaluate these routes in order. Stop as soon as one route has enough evidence
to become the next implementation direction.

#### Route D1: CPU Decoder Fallback

Goal:

```text
Keep the QNN encoder path if useful, but run the Qwen3 decoder on CPU/ORT or
another CPU-capable runtime where KV cache is numerically correct.
```

First validation:

```text
Measure one prompt decode on device CPU:
- init memory
- first token latency
- token/s after KV warmup
- whether output matches ORT reference for 3-step smoke
```

Accept if:

```text
Latency is high but usable enough for a fallback or quality mode, and smoke
matches reference.
```

Reject if:

```text
Memory or latency is clearly unusable on target phone.
```

#### Route D2: Split Decoder / HTP Current Token + CPU KV Attention

Goal:

```text
Keep HTP for parts that work, but move KV-dependent attention/cache math to CPU.
```

First validation:

```text
Write a design doc only. Do not implement until D1 has measured data.
This route likely requires graph surgery and host-side attention, so it is high risk.
```

#### Route D3: Different Mobile Runtime Or Model (RESOLVED)

Goal:

```text
Evaluate lower-latency ASR models/runtimes that can plausibly meet <=1000ms
per segment on-device.
```

First validation:

```text
Paraformer QNN HTP has been selected. The remaining runtime work is fallback and
research:

```text
Fallback: ORT + XNNPACK
Research: Qwen3-ASR LiteRT + Qualcomm Delegate compatibility check
```
```

#### Route D4: Continue QNN HTP Escape Hatch Research

Only continue this if a new primary-source QNN mechanism is identified:

```text
QNN API path that forces input encoding at graphFinalize
HTP-supported graph form that prevents cache input encoding override
documented backend config that changes large-graph IO encoding behavior
```

Do not repeat:

```text
quant_overrides only
--preserve_io datatype on compute graphs
small bridge probes B4-0..B4-6
qnn-net-run on the 912 MB decoder without a new runtime setup hypothesis
```

### Recommended Next Action

This section is historical. Do not create a new CPU fallback plan from this
section. Current actionable work is:

```text
1. Test 10s Paraformer QNN model
2. Add backend selection UI
3. Add ORT + XNNPACK fallback
4. Run Qwen3-ASR LiteRT + Qualcomm Delegate compatibility check only as a
   bounded research probe
```

### Documentation To Update After Gate D

```text
docs/architecture/QWEN3_QNN_DECODER_EXPERIMENT_LOG.md
docs/architecture/QWEN3_QNN_NEXT_TASKS_FOR_AI.md
```

Required result format:

```text
Gate D: LOW_LATENCY_CANDIDATE_SELECTED / NEEDS_MODEL_EVAL / NO_LOCAL_OFFLINE_PATH
Next: <specific implementation route>
```

### Files To Inspect

```text
src/mobile/app/src/main/cpp/qwen3_qnn_backend.cpp
src/mobile/app/src/main/cpp/qwen3_qnn_backend.h
src/mobile/app/src/main/cpp/stt_engine.cpp
third_party/sherpa-onnx-src/sherpa-onnx/csrc/offline-recognizer-qwen3-asr-impl.cc
docs/architecture/QWEN3_QNN_DECODER_EXPERIMENT_LOG.md
docs/architecture/QWEN3_QNN_ANDROID_INTEGRATION_SPEC.md
```

### Reference Implementation

```text
third_party/sherpa-onnx-src/sherpa-onnx/csrc/offline-recognizer-qwen3-asr-impl.cc
```

This file shows how sherpa-onnx handles KV cache, position encoding,
and attention masking for Qwen3-ASR.

## One-Line Task

```text
Gate E proved ORT + XNNPACK Paraformer offline works at 342ms decode (0.061x RTF).
ParaformerQnn is the production path (73ms). ParaformerXnnpack is the non-QNN
fallback (342ms). Qwen3AsrCpu remains as correctness baseline (3313ms).
Next: measure peak RSS, validate with diverse audio, and run the LiteRT
compatibility check.
```
