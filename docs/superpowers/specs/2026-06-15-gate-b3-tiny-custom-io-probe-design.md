# Gate B3: Tiny Custom IO QuantParam Probe — Implementation Design

Date: 2026-06-15

## Problem

Gate A confirmed HTP graphFinalize overrides cache_key/cache_value APP_WRITE
quantization scale to 1.5259e-09. Gate B Route 2 (`--preserve_io datatype`
float32) also failed — HTP converts float32 APP_WRITE inputs back to UFIXED16
with the bad scale.

The remaining converter-side option is `--custom_io` YAML with QuantParam.
The local template warns: "QuantParam Scale/Offset will be ignored if the
precision field for that I/O is not set to uint8." This makes Route 3
low-confidence, but it must be tested before abandoning the converter route.

## Goal

Prove whether `--custom_io` QuantParam can affect 16-bit APP_WRITE input
runtime encoding on this QAIRT/HTP version. Use a tiny ONNX model, not the
870 MB full decoder.

## Architecture

Three phases, executed sequentially:

```
Phase 1 (PC):  Python script builds tiny ONNX → QNN converter with custom_io YAML → libmodel.so
Phase 2 (Android C++):  Load tiny libmodel.so → graphFinalize → audit encoding → run two inputs
Phase 3 (Integration):  Call probe from existing smoke test flow, capture via logcat
```

## Phase 1: Build + Convert

### 1.1 Build Tiny ONNX Model

Script: `scripts/build_tiny_custom_io_probe.py`

Model structure:
```
cache_key_0 [1, 128, 8, 128] float32  →  Identity  →  output_0 [1, 128, 8, 128] float32
```

Why Identity:
- Simplest possible graph — one node, one input, one output
- Output = input, so if HTP reads the buffer, different inputs MUST produce
  different outputs
- No weight constants needed, no MatMul, no extra nodes
- Shape [1, 128, 8, 128] matches real cache_key dimensions

### 1.2 Generate custom_io YAML Variants

Two YAML files for two converter runs:

**Run 1: uint8 with QuantParam**
```yaml
- IOName: cache_key_0
  Datatype: uint8
  QuantParam:
    Type: QNN_DEFINITION_DEFINED
    Scale: 0.015625
    Offset: 0
- IOName: output_0
  Datatype: uint8
  QuantParam:
    Type: QNN_DEFINITION_DEFINED
    Scale: 0.015625
    Offset: 0
```

**Run 2: float32 with QuantParam (control group)**
```yaml
- IOName: cache_key_0
  Datatype: float32
  QuantParam:
    Type: QNN_DEFINITION_DEFINED
    Scale: 0.015625
    Offset: 0
- IOName: output_0
```

Note: Run 2's QuantParam is expected to be ignored per template docs. This
confirms the limitation.

### 1.3 Convert with QNN

Script: `scripts/convert_tiny_custom_io_probe.py`

For each run:
1. `qnn-onnx-converter --input_model tiny_custom_io_probe.onnx --custom_io <yaml> --output_file model.cpp`
2. Build `libmodel.so` for arm64-v8a
3. Verify `model_net.json` shows the intended datatype/quant_params

Output directory structure:
```
G:\STTModels\qnn-work\tiny-custom-io-probe\
  tiny_custom_io_probe.onnx
  custom_io_uint8.yaml
  custom_io_float32.yaml
  run_uint8\
    model_net.json
    libmodel.so
  run_float32\
    model_net.json
    libmodel.so
```

## Phase 2: Android C++ Probe Code

### 2.1 New Method: `runCustomIoProbe()`

Add to `qwen3_qnn_backend.h`:
```cpp
bool runCustomIoProbe(const std::string& modelDir);
```

This is a standalone method that does NOT use the main decoder's QNN
interface. It creates its own QNN backend/device/context/graph for the
tiny model, avoiding interference with the main decoder.

### 2.2 Probe Execution Flow

```
1. Load libmodel.so from modelDir
2. QnnInterfaceGetProviders → obtain QNN interface
3. QnnBackend_create → QnnDevice_create → QnnContext_create
4. QnnModel_composeGraphs (load model.cpp from libmodel.so)
5. QnnGraph_finalize
6. Audit: iterate inputTensors, log dtype/scale/offset for cache_key_0
7. Prepare two input buffers:
   - Buffer A: all zeros (65536 elements × sizeof)
   - Buffer B: linear ramp 0..65535 (or random non-zero pattern)
8. graphExecute with Buffer A → read output_0
9. graphExecute with Buffer B → read output_0
10. Compare: if outputs differ, HTP reads the input buffer
11. Log: dtype, scale, offset, output_diff, verdict
```

### 2.3 Encoding Audit

Reuse existing `logTensorEncoding()` from `qwen3_qnn_backend.cpp` Impl.
Key fields to log:
- `quantizationEncoding` (should be QNN_QUANTIZATION_ENCODING_SCALE_OFFSET)
- `scaleOffsetEncoding.scale`
- `scaleOffsetEncoding.offset`
- `dataType` (QNN_DATATYPE_UFIXED_POINT_16 vs QNN_DATATYPE_FLOAT_32 vs QNN_DATATYPE_UFIXED_POINT_8)

### 2.4 Input Buffer Handling

For uint8 run:
- Buffer size = 1 × 128 × 8 × 128 = 131072 bytes
- Buffer A: all zeros
- Buffer B: 0x55 pattern (alternating bits, non-zero)

For float32 run:
- Buffer size = 131072 × 4 = 524288 bytes
- Buffer A: all zeros
- Buffer B: 1.0f repeated

The probe writes raw bytes directly to the APP_WRITE client buffer,
bypassing `writeFloatInputToTensor` / `quantizeUfixed16`. This tests
whether HTP reads the raw buffer at all, regardless of encoding.

## Phase 3: Integration

### 3.1 Call Entry Point

Add to `stt_engine.cpp` after QNN init succeeds:
```cpp
// Gate B3 probe — runs once, then returns to normal operation
if (getenv("STT_GATE_B3_PROBE")) {
    backend->runCustomIoProbe("/data/local/tmp/tiny-custom-io-probe/run_uint8");
    backend->runCustomIoProbe("/data/local/tmp/tiny-custom-io-probe/run_float32");
}
```

Environment variable gate prevents accidental execution in production.

### 3.2 Deployment Script

Script: `scripts/test_qnn_custom_io_probe.bat`

Steps:
1. `adb push` both `run_uint8/libmodel.so` and `run_float32/libmodel.so`
   to `/data/local/tmp/tiny-custom-io-probe/`
2. Set env var `STT_GATE_B3_PROBE=1`
3. Launch app, capture logcat for `GateB3` tag
4. Parse logcat for verdict

## Acceptance Criteria

```text
PASS: Runtime tensor encoding after HTP finalize keeps a scale that can
      represent the target KV magnitude (scale ≈ 0.015625 or float dtype),
      AND the output changes when the input buffer changes.

FAIL: Runtime encoding finalizes to UFIXED16 scale=1.525902e-09,
      OR QuantParam is ignored except for uint8,
      OR output does not change between Buffer A and Buffer B.
```

On FAIL: stop all converter/custom_io attempts. Move to Gate C.

## Stop Condition

If the tiny custom_io probe still finalizes to UFIXED16 scale=1.525902e-09,
or if QuantParam is ignored except for uint8, stop converter/custom_io
attempts on the full decoder. Move to Gate C and QNN API/config investigation.

## Files to Create/Modify

### New files
- `scripts/build_tiny_custom_io_probe.py` — build tiny ONNX model
- `scripts/convert_tiny_custom_io_probe.py` — convert with custom_io YAML
- `scripts/test_qnn_custom_io_probe.bat` — deploy + run on device

### Modified files
- `src/mobile/app/src/main/cpp/qwen3_qnn_backend.h` — add `runCustomIoProbe()`
- `src/mobile/app/src/main/cpp/qwen3_qnn_backend.cpp` — implement probe
- `src/mobile/app/src/main/cpp/stt_engine.cpp` — add env-var-gated call entry

## Hard Boundaries

- Do not modify the main decoder's init/decode path
- Do not modify protocol.h, src/pc/*, archive/legacy/*
- Do not modify audio_features quantization fix
- Probe uses its own QNN context, separate from the main decoder
