# Gate B4: Bridge Probe For HTP Float32 Override Trigger — Design

Date: 2026-06-15

## Problem

Gate B3 proved a tiny Transpose-only graph with a single float32 APP_WRITE input
survives HTP `graphFinalize` — the runtime encoding stays `QNN_DATATYPE_FLOAT_32`.

But the real 870 MB decoder with `--preserve_io datatype` (Gate B Route 2) still
gets its cache_key/cache_value inputs overridden to `UFIXED16 scale=1.525902e-09`.

Something about the real decoder's graph triggers HTP to override the encoding.
We need to find the smallest graph feature that causes this.

## Goal

Identify the minimum graph feature that causes HTP to override a preserved
`FLOAT_32` APP_WRITE input to `UFIXED16 scale=1.525902e-09`.

Two success paths:

1. **Diagnostic PASS**: a bridge variant reproduces the real decoder behavior
   (converter FLOAT_32 → HTP runtime UFIXED16 scale=1.53e-9). This tells us
   what to fix or avoid.

2. **Recovery PASS**: a constrained variant preserves FLOAT_32 and maps to a
   feasible real decoder rewrite.

## Architecture

### Approach

Extend the Gate B3 pipeline (build_tiny_custom_io_probe.py +
convert_tiny_custom_io_probe.py) with a `--variant` parameter that selects
among five bridge model structures, ordered by increasing similarity to the
real decoder.

Each variant uses `--preserve_io datatype` for all float32 APP_WRITE inputs
(because B3 proved `--custom_io` YAML does not accept float32 as a quantized
type).

After conversion, build libmodel.so, deploy to the phone via qnn-net-run, and
inspect `execution_metadata.yaml` for the runtime dtype/scale of cache_key_0
(and cache_value_0 if present).

### Variant Definitions

#### B4-1: Single float32 input + MatMul

```text
Inputs:
  cache_key_0  FLOAT32  [1, 128, 8, 128]

Graph:
  Reshape(cache_key_0, [1, 1024, 128])    # flatten [H, D] into [seq*heads, head_dim]
  MatMul(reshaped, weight)                 # weight: [128, 64] float32 constant
  Reshape(result, [1, 128, 8, 64])         # back to 4D

Output:
  output_0  FLOAT32  [1, 128, 8, 64]
```

**Why MatMul**: The real decoder has many MatMul ops (Q*K, K*V projections). If
MatMul triggers HTP's quantization propagation, B4-1 will show the override.

#### B4-2: Two float32 inputs + Concat

```text
Inputs:
  cache_key_0    FLOAT32  [1, 128, 8, 128]
  cache_value_0  FLOAT32  [1, 128, 8, 128]

Graph:
  Transpose(cache_key_0, perm=[0,2,1,3])   # [1,8,128,128] NHWC
  Transpose(cache_value_0, perm=[0,2,1,3])  # [1,8,128,128] NHWC
  Concat(key_t, value_t, axis=3)            # [1,8,128,256]

Output:
  output_0  FLOAT32  [1, 8, 128, 256]
```

**Why Concat**: Tests whether having two APP_WRITE float32 inputs triggers the
override. Concat is a simple op that forces HTP to read both buffers.

#### B4-3: 56 float32 inputs + Concat

```text
Inputs:
  cache_key_0..27    FLOAT32  [1, 128, 8, 128]  (28 tensors)
  cache_value_0..27  FLOAT32  [1, 128, 8, 128]  (28 tensors)

Graph:
  Transpose each to NHWC [1,8,128,128]
  Concat(all 56, axis=3) -> [1,8,128,7168]

Output:
  output_0  FLOAT32  [1, 8, 128, 7168]
```

**Why 56 inputs**: The real decoder has 56 cache inputs (28 key + 28 value).
If HTP treats multiple float32 inputs differently, B4-3 will show the override.

Note: This model will be larger due to 56 input tensors in the graph, but
should still be tiny compared to the real decoder.

#### B4-4: 56 float32 inputs + delta outputs

```text
Inputs:
  cache_key_0..27    FLOAT32  [1, 128, 8, 128]
  cache_value_0..27  FLOAT32  [1, 128, 8, 128]

Graph:
  For each layer i:
    Transpose(cache_key_i) -> key_nchw_i
    Transpose(cache_value_i) -> val_nchw_i
    Add(key_nchw_i, delta_const) -> key_delta_i
    Add(val_nchw_i, delta_const) -> value_delta_i

Outputs:
  key_delta_0..27    FLOAT32  [1, 128, 8, 128]
  value_delta_0..27  FLOAT32  [1, 128, 8, 128]
  logits             FLOAT32  [1]  (ReduceSum of first key_delta)
```

**Why delta outputs**: The real decoder outputs key_delta and value_delta per
layer plus logits. If the presence of many quantized-output tensors triggers
HTP to re-quantize the input path, B4-4 will show the override.

#### B4-5: Minimal attention-shaped subgraph

```text
Inputs:
  cache_key_0    FLOAT32  [1, 128, 8, 128]
  rope_emb       FLOAT32  [1, 64]

Graph:
  Transpose(cache_key_0, [0,2,1,3]) -> [1,8,128,128]  (NHWC)
  Split(key, axis=2, splits=[64,64]) -> key_rot, key_pass
  Mul(key_rot, rope_cos) -> rotated
  Concat(key_pass, rotated, axis=2) -> applied_rope
  MatMul(applied_rope, weight) -> attn_out    # weight [128,64]

Output:
  output_0  FLOAT32  [1, 8, 128, 64]
```

**Why attention-shaped**: The real decoder applies RoPE to cache_key, then uses
it in attention. If the RoPE/MatMul combination is the trigger, B4-5 will
show it. This is the most complex variant but still tiny.

### Decision Logic

```text
If B4-1 shows override → trigger is MatMul (or any compute op on float32 input)
If B4-2 shows override → trigger is multiple float32 inputs
If B4-3 shows override → trigger is input count (56 inputs)
If B4-4 shows override → trigger is output delta structure
If B4-5 shows override → trigger is attention/RoPE structure
If none show override → trigger is graph size/weight count (beyond B4 scope)
```

### Stop Condition

If no B4 variant reproduces the override within one focused pass, stop
converter/custom_io work. Proceed to Gate C.

### Output Per Variant

For each variant that successfully runs on device:

```text
execution_metadata.yaml:
  - cache_key_0: datatype, dimensions
  - cache_value_0 (if present): datatype, dimensions
  - output_0: datatype, dimensions

converter model_net_summary.json:
  - cache_key_0: data_type, scale, offset, is_overridden
  - output_0: data_type, scale, offset, is_overridden

Zero vs pattern input comparison:
  - output_0.raw byte difference
```

## Files To Modify

| File | Action | Responsibility |
|------|--------|----------------|
| `scripts/build_tiny_custom_io_probe.py` | Modify | Add `--variant b4-1..b4-5` with different model builders |
| `scripts/convert_tiny_custom_io_probe.py` | Modify | Support multi-input variants, generate preserve_io args dynamically |
| `scripts/run_gate_b3_probe.ps1` | Modify | Extend for B4 variant names and multi-input test |

No APK changes planned.

## Hard Boundaries

Same as task document. Do not modify:
- qwen3_qnn_backend.cpp/h
- stt_engine.cpp
- protocol.h, src/pc/*
- QNN init path, audio_features quantization fix, W=4 smoke gate

Do not rerun a fulldecoder conversion unless B4 identifies a concrete
converter flag, graph rewrite, or input/output form.
