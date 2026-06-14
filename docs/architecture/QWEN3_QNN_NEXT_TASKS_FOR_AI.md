# Qwen3 QNN Next Tasks For AI

Last updated: 2026-06-15

This is the active handoff for the next AI worker. It replaces older appended task lists.

## Read First

```text
docs/architecture/QWEN3_QNN_DECODER_EXPERIMENT_LOG.md
docs/architecture/QWEN3_QNN_ANDROID_INTEGRATION_SPEC.md
docs/architecture/QWEN3_QNN_DIAGNOSIS_REPORT.md
third_party/sherpa-onnx-src/sherpa-onnx/csrc/offline-recognizer-qwen3-asr-impl.cc
```

## Current Status

```text
Gate 1: QNN init smoke                 PASSED
Gate 2: W=4 in-app smoke gate          PASSED
Gate 3: audio_features quant fix       PASSED
Gate 4: sherpa generation cleanup      PASSED
Gate 5: real-audio E2E with W=4        FAILED
Gate 6: W=128 decoder build + init     PASSED
Gate 7: real-audio E2E with W=128      FAILED (identical garbage to W=4)
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
```

Current conclusion (updated Step 21):

```text
kv_ends=1 is NOT the root cause. The prewindow decoder already removed all Slice nodes.
The W=128 KV window is directly fed to the attention mechanism.

ROOT CAUSE: HTP graphFinalize overrides cache_key_N / cache_value_N quantization scale.
  model.cpp defines:    scale = 0.015625 (min=-512, max=512)
  quant_overrides:      scale = 0.015625, is_overridden=True
  model_net.json:       is_overridden=True, scale=0.015625
  HTP runtime actual:   scale = 1.5259021824e-09 (range [-0.0001, 0.0001])

With scale=1.53e-9, all real KV data (key_absmax~330) is quantized to 0.
This is why KV Influence Probe fails: HTP literally sees zero KV regardless of input.
This is why smoke test passes: position_scalar and token differences still produce
different logits even with zero KV (current-token attention path works).

The problem is at the C++ runtime / QNN HTP quantization propagation level.
model conversion (quant_overrides, calibration data) cannot fix it.

NEXT STEP: Determine whether HTP APP_WRITE input tensor encodings can be
controlled or bypassed. Raw uint16 and pre-scaling are diagnostic probes only
unless they preserve the real KV magnitude needed by Qwen3 attention.
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

## Next Task: Determine Whether HTP KV Input Encoding Can Be Controlled

Gate 1 (KV Influence Probe) failed because HTP graphFinalize overrides
cache_key/cache_value quantization scale to 1.53e-9, making all KV data
invisible. The prewindow decoder (Slice removed) is architecturally correct,
but the quantization propagation in HTP prevents KV data from reaching
the attention mechanism.

Do not treat raw uint16 writes, pre-scaling, or manual quantization as final
fixes until they prove that Qwen3 can see KV values at a useful magnitude.
With HTP scale fixed at about `1.53e-9`, a uint16 tensor can only represent
roughly `0..0.0001` after dequantization. That is far below observed
`key_delta` magnitudes in the hundreds.

### Gate A: Runtime Encoding Audit

Goal: capture the tensor encoding HTP uses after `QnnGraph_finalize`, from the
same tensor descriptors used for `graphExecute`.

Required output:

```text
For at least cache_key_0, cache_key_27, cache_value_0, cache_value_27:
- tensor name
- data type
- bitwidth
- scale
- offset
- min/max if available
- tensor dimensions
```

Acceptance:

```text
The log clearly shows whether HTP runtime encoding matches model_net.json or
uses the bad 1.525902e-09 scale.
```

If runtime encoding matches `model_net.json`, stop this task and return to
input binding/layout diagnosis. If runtime encoding still uses `1.53e-9`,
continue to Gate B.

### Gate B: Context Binary Or Converter Route

Goal: test one route that may preserve KV input encodings through HTP finalize.

Try these in order, stopping after the first route gives correct runtime
encoding:

1. Generate and load a QNN context binary for the W=128 prewindow decoder.
2. Try QAIRT/QNN converter custom IO or encoding options that force APP_WRITE
   input encodings for `cache_key_N/cache_value_N`.
3. Try float32 APP_WRITE cache inputs only if QNN HTP accepts the graph and the
   artifact remains deployable.

Acceptance:

```text
Runtime tensor encoding for cache_key/cache_value matches the intended range:
- key scale around 0.015625 or another range that can represent key_absmax~330
- value scale large enough for observed value_absmax, up to about 64 in Step 21 calibration
```

Then rerun:

```text
KV Influence Probe
Decoder smoke test
```

If KV Influence Probe passes, proceed to first-step reference alignment. If it
still fails despite correct runtime encoding, investigate APP_WRITE buffer
binding and layout.

### Gate C: Diagnostic Raw-Buffer Probe

Goal: determine whether HTP reads the APP_WRITE cache buffers at all.

This is not a final quality fix.

Run two probes:

```text
Probe 1: write all-zero raw cache buffers
Probe 2: write max-pattern raw uint16 cache buffers
```

Acceptance:

```text
If logits change, HTP reads the cache buffers but scale/range is unusable.
If logits do not change, focus on input binding, graph connectivity, or layout.
```

### Files To Inspect

```text
src/mobile/app/src/main/cpp/qwen3_qnn_backend.cpp
src/mobile/app/src/main/cpp/qwen3_qnn_backend.h
src/mobile/app/src/main/cpp/stt_engine.cpp
third_party/sherpa-onnx-src/sherpa-onnx/csrc/offline-recognizer-qwen3-asr-impl.cc
docs/architecture/QWEN3_QNN_DECODER_EXPERIMENT_LOG.md
```

### Reference Implementation

```text
third_party/sherpa-onnx-src/sherpa-onnx/csrc/offline-recognizer-qwen3-asr-impl.cc
```

This file shows how sherpa-onnx handles KV cache, position encoding,
and attention masking for Qwen3-ASR.

## One-Line Task

```text
Determine whether QNN HTP can preserve or bypass usable cache_key/cache_value
input encodings. First audit finalized runtime encodings, then test context
binary/custom-IO/float32 routes, and use raw-buffer writes only as a diagnostic
probe for APP_WRITE buffer connectivity.
```
