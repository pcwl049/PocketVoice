# Qwen3 QNN HTP KV Cache Encoding Audit Design

Date: 2026-06-15

## Problem

HTP `graphFinalize` overrides `cache_key/cache_value` APP_WRITE input tensor
quantization scales from the intended 0.015625 to 1.5259e-09.  This makes all
KV data invisible to the attention mechanism, causing garbage E2E output.

## Goal (Gate A)

Capture the actual tensor encoding HTP uses after `QnnGraph_finalize` for
`cache_key_0`, `cache_key_27`, `cache_value_0`, `cache_value_27`.

## Design

### Change: Call `auditRuntimeEncodings()` at end of `init()`

The method already exists (lines 1749-1919 in `qwen3_qnn_backend.cpp`) with
two audit paths:

- **Path 1**: Read `quantizeParams` from stored `GraphInfo.inputTensors`
  structs (post-finalize in-memory values).
- **Path 2**: Extract context binary via `QnnSystemContext_getMetaData` (or
  fallback `getBinaryInfo`) and introspect encoding from binary metadata.

### Placement

After graphFinalize succeeds and before the first decode step, in `init()`:

```
graphFinalize → allocateKVCache → auditRuntimeEncodings()
```

### Required Log Output

For each of `cache_key_0`, `cache_key_27`, `cache_value_0`, `cache_value_27`:

```
- tensor name
- data type (UFIXED_POINT_16, FLOAT_32, etc.)
- bitwidth
- scale
- offset
- min/max (if available)
- tensor dimensions
```

### Acceptance

The log clearly shows whether HTP runtime encoding matches `model_net.json`
(scale ≈ 0.015625) or uses the bad scale (1.5259e-09).

### Decision Branch

- **scale ≈ 0.015625** → stop; pivot to input binding/layout diagnosis
- **scale ≈ 1.53e-9**  → proceed to Gate B (context binary / converter / float32)

## Scope

- Only `qwen3_qnn_backend.cpp` — add one call in `init()`
- No new files, no new JNI methods
- Diagnostic only — no behavioral changes to decode path

## Gates B and C (future, not in this change)

- **Gate B**: Test context binary, converter custom IO, or float32 APP_WRITE
- **Gate C**: Raw-buffer probe (zero vs max-pattern) to confirm APP_WRITE
  buffer connectivity
