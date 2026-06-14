# Gate B: Context Binary / Converter / Float32 Routes Design

Date: 2026-06-15

## Problem

Gate A confirmed: HTP graphFinalize overrides cache_key/cache_value APP_WRITE
quantization scale from 0.015625 to 1.5259e-09 on **both** Path 1 (GraphInfo
stored structs) and Path 2 (context binary metadata). All KV data is quantized
to zero, making it invisible to the attention mechanism.

## Goal

Find a route that preserves usable cache_key/cache_value input encodings
through HTP finalize. Required: key scale that can represent key_absmax~330,
value scale that can represent value_absmax~64.

## Routes (tried in order, stop after first success)

### Route 1: Context Binary Load Verification

**Hypothesis**: Loading a pre-compiled context binary via
`QnnContext_createFromBinary` may preserve different quantization encodings
than the composeGraphs+graphFinalize path.

**Implementation**:
1. After graphFinalize, save context binary to persistent file
2. Add alternative init path: if context binary file exists, load via
   `contextCreateFromBinary` + `graphRetrieve`
3. Run `auditRuntimeEncodings()` after loading from binary

**Expected outcome**: Scale will likely still be 1.53e-9 (context binary
captures post-finalize state, which already has the bad scale). This route
is primarily a verification, not a fix.

**Acceptance**: Runtime encoding scale ≠ 1.53e-9 for cache_key/cache_value.

### Route 2: Float32 APP_WRITE via `--preserve_io datatype`

**Hypothesis**: If cache_key/cache_value are declared as float32 APP_WRITE
inputs, HTP will insert Cast(float32→float16) ops at graph boundaries instead
of quantizing with an inappropriate scale. The user provides float32 data,
HTP converts to float16 internally.

**Implementation**:
1. Modify conversion script: add `--preserve_io datatype cache_key_0
   cache_key_1 ... cache_value_27` to converter command
2. Re-convert ONNX → model.cpp → libmodel.so
3. Modify C++ runtime: write float32 data directly to cache_key/cache_value
   buffers (skip quantizeUfixed16), handle float32 output for key_delta/
   value_delta (skip dequantizeValue)
4. Deploy, run audit, run KV Influence Probe

**Key QNN docs**: "QNN HTP supports running graphs having a mix of floating-
point and fixed-point data types." Float32 APP_WRITE inputs are possible;
HTP inserts Cast ops at boundaries. Float32 math is NOT supported — actual
computation is float16.

**Trade-offs**:
- Pro: Bypasses quantization scale problem entirely
- Con: Cast overhead on every inference for 56 large tensors
- Con: float16 precision may lose detail vs int16 with correct scale
- Con: key_delta/value_delta outputs also become float32 (or float16 with cast)

**Acceptance**: KV Influence Probe passes (diff > 1.0 between zero and
non-zero KV inputs).

### Route 3: Custom IO YAML with QuantParam

**Hypothesis**: `--custom_io` YAML may force specific Scale/Offset values on
APP_WRITE inputs that survive HTP finalize.

**Implementation**:
1. Generate custom_io YAML with QuantParam for each cache_key/cache_value
2. Re-convert with `--custom_io`
3. Deploy and audit

**Risk**: Custom IO QuantParam may conflict with quantization_overrides.
Documentation says custom_io only supports int8/uint8 QuantParam for
quantized inputs.

**Acceptance**: Same as Route 2.

## Decision Tree

```
Route 1 → scale changed? ──YES──→ Rerun KV probe → probe passes? ──YES──→ DONE
         └──NO──→ Route 2
Route 2 → graph accepts float32? ──NO──→ Route 3
         └──YES──→ audit scale ──float32/float16──→ KV probe ──passes? ──YES──→ DONE
                                                     └──fails──→ Route 3
```

## Scope

Files to modify:
- `src/mobile/app/src/main/cpp/qwen3_qnn_backend.cpp` — context binary load,
  float32 input/output handling
- `src/mobile/app/src/main/cpp/qwen3_qnn_backend.h` — new method signatures
- `scripts/convert_qwen3_decoder_single_model_kv_override.py` — preserve_io
  or custom_io flags

## Hard Boundaries

Same as task document:
- Do not modify protocol.h, src/pc/*, archive/legacy/*
- Do not modify QNN init path (only add alternative path)
- Do not modify audio_features quantization fix
