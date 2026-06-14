# Qwen3 QNN Next Tasks For AI

Date: 2026-06-14

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
```

Current conclusion:

```text
KV cache quantization override is FIXED (Step 19).
quant_overrides_v4.json correctly targets cache_key_0..27 and cache_value_0..27.
model_net.json shows is_overridden=True with correct ranges.
KV cache data is now non-zero (cache_nonzero grows from 0 to 6.9M).

However, real-audio E2E still produces garbage output.
The garbage is NOT byte-identical to the previous W=4/W=128 output —
different tokens are selected, confirming the KV fix changed decoder behavior.

Remaining problems (in order of likelihood):
1. KV cache window alignment — host must correctly slice the active W=128
   window from the full KV history and set attention_bias masking.
2. RoPE / position encoding — rope_emb and position_scalar may not be
   correctly computed for the current step within the W=128 window.
3. NHWC layout mismatch — QNN uses NHWC layout, Android code must pack
   KV data in NHWC order.
4. Audio features integration — encoder output may not reach the decoder
   at the correct graph input position.

The KV quantization fix is confirmed and should NOT be reverted.
Next diagnosis should focus on the Android runtime KV cache management.
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
Result: byte-identical multilingual garbage compared with W=4
```

Key generation evidence:

```text
[DIAG-gen] step=0, input=198, argmax=14101, selected=14101
[DIAG-gen] step=10, input=62, argmax=20136, selected=20136
[DIAG-gen] step=11..15 repeats token 20136
[DIAG-gen] stop at step=42 token=151643
```

W=128 did not change the output. That rules out "W=4 prompt context loss" as
the primary current explanation. Step 18 identified the stronger blocker:

```text
cache_key_0..27 and cache_value_0..27 inputs did not receive KV quant overrides.
They still use default near-zero quantization range [0, 0.0001].
KV information is effectively destroyed before it reaches the decoder graph.
```

The next worker should fix KV input quantization before doing more window-size
or prompt-generation experiments.

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
```

## Next Task: Fix Android Runtime KV Cache Management

KV quantization is fixed. The remaining E2E garbage is likely caused by
incorrect runtime KV cache handling on the Android side.

### Diagnosis

The decoder prompt structure is correct:
```
<|im_start|>user\n<|im_end|>\n
<|im_start|>assistant\n<audio_start><audio_pad>×65<audio_end><|im_end|>\n
<|im_start|> [garbage tokens...]
```

But the generated tokens after the prompt are incoherent. This suggests
the decoder receives audio features correctly but fails to produce
correct speech recognition output.

### Likely Root Causes (ordered by priority)

1. **KV cache window alignment**: The host manages a full KV history
   buffer but QNN only sees W=128 positions. The host must correctly
   slice the active window, copy KV deltas at the right position, and
   set `attention_bias` to mask unfilled past positions. Any misalignment
   here would cause the decoder to attend to wrong KV history.

2. **RoPE / position encoding**: `rope_emb` and `position_scalar`
   inputs may not be computed correctly for the current generation step
   within the W=128 window. Wrong position information would corrupt
   attention computation.

3. **NHWC layout mismatch**: QNN uses NHWC layout (--preserve_io layout).
   The Android code must pack KV cache data in NHWC order. If packed
   in NCHW, the decoder would read swapped dimensions.

### Required Work

1. Trace the exact data flow from encoder output to decoder input on
   Android, comparing with the sherpa-onnx reference implementation.

2. Verify KV delta write-back: after each decode step, the decoder
   outputs `key_delta_N` and `value_delta_N`. The host must add these
   deltas to the full KV history buffer at the correct position index.

3. Verify KV window slicing: when preparing inputs for the next decode
   step, the host must copy the last W positions from the full KV history
   into `cache_key_N` / `cache_value_N` graph inputs.

4. Verify `attention_bias` computation: must mask unfilled positions
   in the W=128 window with -10000, and unmask filled positions.

5. Verify `rope_emb` and `position_scalar`: must match the current
   absolute position within the full sequence.

6. Compare each input tensor against the sherpa-onnx reference for the
   same audio and prompt.

### Reference Implementation

```text
third_party/sherpa-onnx-src/sherpa-onnx/csrc/offline-recognizer-qwen3-asr-impl.cc
```

This file shows how sherpa-onnx handles KV cache, position encoding,
and attention masking for Qwen3-ASR.

## One-Line Task

```text
Diagnose and fix the Android runtime KV cache management to ensure
correct window slicing, position encoding, and attention masking,
using sherpa-onnx reference as the baseline.
```
