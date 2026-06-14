# Gate B: Float32 APP_WRITE KV Cache Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Convert the Qwen3 decoder with `--preserve_io datatype` for cache_key/cache_value tensors so HTP accepts float32 KV cache inputs, bypassing the broken quantization scale problem.

**Architecture:** Modify the ONNX-to-QNN conversion script to add `--preserve_io datatype cache_key_0 ... cache_value_27`. This causes the QNN converter to declare these inputs as float32 APP_WRITE. HTP inserts Cast(float32→float16) ops at graph boundaries. The C++ runtime already has float32 input/output paths — no code changes needed in the runtime.

**Tech Stack:** Python (QAIRT converter), PowerShell (libmodel.so build), Android ADB (deploy/test)

---

## Key Insight: C++ Runtime Already Supports Float32

The existing `writeFloatInputToTensor()` at `qwen3_qnn_backend.cpp:413-438` has a
`QNN_DATATYPE_FLOAT_32` branch that does a plain `memcpy`. The existing
`readTensorToFloat()` at `qwen3_qnn_backend.cpp:506-527` also has a float32
branch. The `tensorElementSize()` function returns 4 for `QNN_DATATYPE_FLOAT_32`.
**No C++ code changes are required for the runtime path.**

---

### Task 1: Verify converter `--preserve_io datatype` syntax

**Files:**
- Inspect: `G:\Program Files\qairt\2.45.0.260326\bin\x86_64-windows-msvc\qnn-onnx-converter`

- [ ] **Step 1: Run converter help to confirm `--preserve_io` syntax**

Run:
```bash
PYTHON="D:/Project/STT/build/qairt-py310-venv/Scripts/python.exe"
CONVERTER="G:/Program Files/qairt/2.45.0.260326/bin/x86_64-windows-msvc/qnn-onnx-converter"
$PYTHON $CONVERTER --help 2>&1 | grep -A5 "preserve_io"
```

Expected: Help text shows `--preserve_io` accepts `layout`, `datatype`, or
tensor names. Record exact syntax.

- [ ] **Step 2: Verify `--preserve_io datatype` accepts a list of tensor names**

If the help shows `--preserve_io datatype` (preserve all IO) or
`--preserve_io datatype tensor1 tensor2` (preserve specific tensors), record
the format. If the syntax is different, adjust Task 2 accordingly.

- [ ] **Step 3: Commit the recorded syntax as a note**

No file changes — just verify and proceed. If syntax is wrong, stop and ask.

---

### Task 2: Modify conversion script for float32 cache_key/cache_value

**Files:**
- Modify: `scripts/convert_qwen3_decoder_single_model_kv_override.py`

- [ ] **Step 1: Add `--preserve_io datatype` arguments for KV cache tensors**

In `convert_qwen3_decoder_single_model_kv_override.py`, add a new function
`preserve_io_datatype_args()` that returns a list of `--preserve_io datatype`
entries for all 56 KV cache tensors. Then add these entries to the converter
command.

Add after `input_dtype_overrides()` (around line 46):

```python
def preserve_io_datatype_args():
    """Return --preserve_io datatype args for cache_key/cache_value tensors."""
    args = []
    for i in range(NUM_LAYERS):
        args.extend(["--preserve_io", "datatype", f"cache_key_{i}"])
        args.extend(["--preserve_io", "datatype", f"cache_value_{i}"])
    return args
```

In `main()`, after the `--preserve_io layout` entry (line 120), add the
preserve_datatype args. The converter command section (lines 111-123) becomes:

```python
    cmd = [
        str(PYTHON), str(CONVERTER),
        "--input_network", str(MODEL),
        "--output_path", str(out_dir / "model.cpp"),
        "--input_list", str(INPUT_LIST),
        "--quantization_overrides", str(override_file),
        "--act_bitwidth", "16",
        "--weights_bitwidth", "8",
        "--bias_bitwidth", "32",
        "--preserve_io", "layout",
    ]
    cmd.extend(preserve_io_datatype_args())
    cmd.extend(input_dtype_overrides())
    cmd.extend(input_dims())
```

Note: `--preserve_io datatype` for specific tensors takes precedence over
`--preserve_io layout` for all tensors. The layout is still preserved for
non-KV-cache IO.

Also update the default variant name to indicate float32 KV:

```python
    parser.add_argument("--variant", default="qwen3-decoder-fullkv-act16-single-model-f32kv-i32")
```

- [ ] **Step 2: Verify the script changes**

Read the modified file and confirm:
1. `preserve_io_datatype_args()` generates correct `--preserve_io datatype cache_key_0` through `cache_value_27`
2. The args are inserted into the command after `--preserve_io layout`
3. The variant name includes `f32kv`

- [ ] **Step 3: Commit the conversion script change**

```bash
git add scripts/convert_qwen3_decoder_single_model_kv_override.py
git commit -m "feat: add --preserve_io datatype for float32 KV cache inputs"
```

---

### Task 3: Run the conversion

**Files:** None (run conversion script)

- [ ] **Step 1: Run the conversion script**

Run:
```bash
cd D:/Project/STT
PYTHON="D:/Project/STT/build/qairt-py310-venv/Scripts/python.exe"
$PYTHON scripts/convert_qwen3_decoder_single_model_kv_override.py --variant qwen3-decoder-fullkv-act16-single-model-f32kv-i32
```

Expected: Conversion completes without error. The output directory
`G:\STTModels\qnn-work\qnn-convert\qwen3-decoder-fullkv-act16-single-model-f32kv-i32\`
contains `model.cpp`, `model.bin`, and `model_net.json`.

This step may take 5-20 minutes depending on the model size.

- [ ] **Step 2: Inspect model_net.json for float32 cache_key/cache_value**

Run:
```bash
cd "G:/STTModels/qnn-work/qnn-convert/qwen3-decoder-fullkv-act16-single-model-f32kv-i32"
python -c "
import json
d = json.load(open('model_net.json', encoding='utf-8'))
for name in ['cache_key_0', 'cache_value_0', 'cache_key_27', 'cache_value_27']:
    t = d['graph']['tensors'][name]
    print(f'{name}: data_type={t[\"data_type\"]}, definition={t.get(\"definition\",\"?\")}, quant_params={t.get(\"quant_params\",{})}')
"
```

Expected: `data_type` for cache_key/cache_value tensors should be `1` (float32)
or similar, NOT `1046` (quantized int16). If still `1046`, the `--preserve_io
datatype` flag did not work — investigate before proceeding.

- [ ] **Step 3: Record findings**

If conversion fails or data_type is still quantized, record the error and
investigate. If data_type shows float32, proceed to Task 4.

---

### Task 4: Build libmodel.so and APK

**Files:** None (build steps)

- [ ] **Step 1: Build libmodel.so with the new variant**

Run:
```powershell
cd D:\Project\STT
scripts\build_qnn_model_lib_android_decoder_fullkv.ps1 -Variant qwen3-decoder-fullkv-act16-single-model-f32kv-i32
```

Set env var first if needed:
```powershell
$env:QNN_SDK_ROOT = "G:\Program Files\qairt\2.45.0.260326"
$env:NDK_PATH = "D:\Android\Sdk\ndk\35.0.0"
```

Expected: `libmodel.so` built at
`D:\Project\STT\build\qnn-model-lib-android\qwen3-decoder-fullkv-act16-single-model-f32kv-i32\libs\arm64-v8a\libmodel.so`

- [ ] **Step 2: Set the environment variable for the APK build**

The APK build script uses `STT_SENSEVOICE_QNN_LIBMODEL` to override which
libmodel.so to include. Set this to the new variant's libmodel.so path.

- [ ] **Step 3: Build and install the APK**

Run:
```powershell
$env:ANDROID_SDK_ROOT = "D:\Android\Sdk"
$env:ANDROID_HOME = "D:\Android\Sdk"
$env:STT_SENSEVOICE_QNN_LIBMODEL = "D:\Project\STT\build\qnn-model-lib-android\qwen3-decoder-fullkv-act16-single-model-f32kv-i32\libs\arm64-v8a\libmodel.so"
scripts\build_mobile_apk.bat
```

Then install:
```bash
"D:/Android/Sdk/platform-tools/adb.exe" install -r "D:/Project/STT/build/mobile-apk/app-signed.apk"
```

Expected: APK installed successfully.

- [ ] **Step 4: Clear logcat, launch app, collect audit**

Run:
```bash
ADB="D:/Android/Sdk/platform-tools/adb.exe"
$ADB logcat -c
$ADB shell am start -n com.stt.mobile/.MainActivity
# Wait 45 seconds for QNN init + audit to complete
sleep 45
$ADB logcat -d | grep "Qwen3Qnn" > D:/Project/STT/build/gate_b_f32kv_audit.log
```

- [ ] **Step 5: Analyze audit results**

Read `build/gate_b_f32kv_audit.log` and check:

1. **Did graphFinalize succeed?** Look for `Graph 'model' finalized` line.
2. **What is the dtype for cache_key_0, cache_value_0?** Look for
   `input[6]: cache_key_0 (id=..., type=0, dtype=???)`. If dtype is `1` or
   shows `FLOAT32`, the conversion worked.
3. **What does Path 1/Path 2 audit show?** Look for `scale=` values. If
   cache_key shows `FLOAT32` dtype (not `UFIXED16`), the scale problem is
   bypassed.
4. **Did the smoke test pass?**
5. **Did the KV Influence Probe pass?**

Decision:
- If dtype=FLOAT32 for cache_key/cache_value AND KV probe passes → **SUCCESS**
- If dtype=FLOAT32 but KV probe fails → investigate input binding
- If dtype still shows UFIXED16/1046 → `--preserve_io datatype` did not work, try Route 3

---

### Task 5: Record results in experiment log

**Files:**
- Modify: `docs/architecture/QWEN3_QNN_DECODER_EXPERIMENT_LOG.md`
- Modify: `docs/architecture/QWEN3_QNN_NEXT_TASKS_FOR_AI.md`

- [ ] **Step 1: Append Step 23 to experiment log**

Append a new section to `QWEN3_QNN_DECODER_EXPERIMENT_LOG.md`:

```text
## Step 23: Gate B Route 2 — Float32 APP_WRITE KV Cache

**Goal**: Test whether `--preserve_io datatype` for cache_key/cache_value
bypasses the HTP quantization scale override problem.

**Method**: Modified converter script to add `--preserve_io datatype
cache_key_0 ... cache_value_27`. Re-converted and deployed.

**Results**: [fill in from audit log]

**Conclusion**: [fill in based on results]
```

- [ ] **Step 2: Update task doc gate status**

Update `QWEN3_QNN_NEXT_TASKS_FOR_AI.md` with Gate B result.

- [ ] **Step 3: Commit**

```bash
git add -f docs/architecture/QWEN3_QNN_DECODER_EXPERIMENT_LOG.md docs/architecture/QWEN3_QNN_NEXT_TASKS_FOR_AI.md
git commit -m "docs: record Gate B Route 2 float32 KV cache results"
```
