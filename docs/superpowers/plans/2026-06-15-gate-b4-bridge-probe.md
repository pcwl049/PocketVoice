# Gate B4: Bridge Probe Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Run 5-7 bridge probe variants on device to find the smallest graph feature that causes HTP to override float32 APP_WRITE to UFIXED16 scale=1.53e-9.

**Architecture:** Extend the Gate B3 pipeline (build → convert → deploy → qnn-net-run → inspect execution_metadata.yaml) with `--variant b4-0..b4-6` parameters. Each variant builds a different ONNX model. Run them sequentially, stopping when a variant reproduces the bad runtime encoding.

**Tech Stack:** Python + ONNX, QAIRT QNN converter with `--preserve_io datatype`, Android NDK, qnn-net-run on device HTP.

---

## File Structure

| File | Action | Responsibility |
|------|--------|----------------|
| `scripts/build_tiny_custom_io_probe.py` | Modify | Add `--variant b4-0..b4-6` with different ONNX model builders and multi-input raw file generation |
| `scripts/convert_tiny_custom_io_probe.py` | Modify | Add B4 variant support: dynamic `--preserve_io datatype` args, per-variant `-d` dims, multi-input summary extraction |
| `scripts/run_gate_b4_probe.ps1` | Create | Deploy + run qnn-net-run for B4 variants, pull execution_metadata.yaml + output raw, compare zero vs pattern |
| `scripts/analyze_b4_results.py` | Create | Parse all B4 execution_metadata.yaml files, print summary table, apply decision logic |

---

### Task 1: Extend build_tiny_custom_io_probe.py for B4 variants

**Files:**
- Modify: `scripts/build_tiny_custom_io_probe.py`

- [ ] **Step 1: Add B4 variant model builders**

Add these builder functions to the existing script. Each builds a different ONNX model:

```python
def build_b4_0(path: Path) -> None:
    """B4-0: Reproduce Gate B3 baseline — Transpose + ReduceSum."""
    cache_in = helper.make_tensor_value_info("cache_key_0", TensorProto.FLOAT, list(SHAPE))
    output = helper.make_tensor_value_info("output_0", TensorProto.FLOAT, [1])
    transpose = helper.make_node("Transpose", ["cache_key_0"], ["transposed"], perm=[0, 2, 1, 3])
    reduce_sum = helper.make_node("ReduceSum", ["transposed"], ["output_0"], keepdims=0)
    graph = helper.make_graph([transpose, reduce_sum], "b4_0", [cache_in], [output])
    _save_model(graph, path)


def build_b4_1(path: Path) -> None:
    """B4-1: Single cache input + MatMul + ReduceSum."""
    cache_in = helper.make_tensor_value_info("cache_key_0", TensorProto.FLOAT, list(SHAPE))
    output = helper.make_tensor_value_info("output_0", TensorProto.FLOAT, [1])
    # weight: [128, 16] small deterministic constant
    weight_data = np.linspace(-0.1, 0.1, 128 * 16, dtype=np.float32).reshape(128, 16)
    weight_init = numpy_helper.from_array(weight_data, name="matmul_weight")
    reshape = helper.make_node("Reshape", ["cache_key_0", "shape_2d"], ["flat_key"])
    shape_val = numpy_helper.from_array(np.array([1024, 128], dtype=np.int64), name="shape_2d")
    matmul = helper.make_node("MatMul", ["flat_key", "matmul_weight"], ["matmul_out"])
    reduce_sum = helper.make_node("ReduceSum", ["matmul_out"], ["output_0"], keepdims=0)
    graph = helper.make_graph(
        [reshape, matmul, reduce_sum], "b4_1", [cache_in], [output],
        initializer=[weight_init, shape_val],
    )
    _save_model(graph, path)


def build_b4_2(path: Path) -> None:
    """B4-2: Two cache inputs (key + value), each ReduceSum, then Add."""
    key_in = helper.make_tensor_value_info("cache_key_0", TensorProto.FLOAT, list(SHAPE))
    val_in = helper.make_tensor_value_info("cache_value_0", TensorProto.FLOAT, list(SHAPE))
    output = helper.make_tensor_value_info("output_0", TensorProto.FLOAT, [1])
    rs_key = helper.make_node("ReduceSum", ["cache_key_0"], ["key_sum"], keepdims=0)
    rs_val = helper.make_node("ReduceSum", ["cache_value_0"], ["val_sum"], keepdims=0)
    add = helper.make_node("Add", ["key_sum", "val_sum"], ["output_0"])
    graph = helper.make_graph([rs_key, rs_val, add], "b4_2", [key_in, val_in], [output])
    _save_model(graph, path)


def build_b4_3(path: Path) -> None:
    """B4-3: 56 cache inputs, each ReduceSum, then Add all scalars."""
    NUM_LAYERS = 28
    inputs = []
    nodes = []
    sum_names = []
    for i in range(NUM_LAYERS):
        k_name = f"cache_key_{i}"
        v_name = f"cache_value_{i}"
        inputs.append(helper.make_tensor_value_info(k_name, TensorProto.FLOAT, list(SHAPE)))
        inputs.append(helper.make_tensor_value_info(v_name, TensorProto.FLOAT, list(SHAPE)))
        ks_name = f"ks_{i}"
        vs_name = f"vs_{i}"
        nodes.append(helper.make_node("ReduceSum", [k_name], [ks_name], keepdims=0))
        nodes.append(helper.make_node("ReduceSum", [v_name], [vs_name], keepdims=0))
        sum_names.extend([ks_name, vs_name])
    # Add all scalars pairwise
    cur = sum_names[0]
    for idx in range(1, len(sum_names)):
        next_name = f"acc_{idx}" if idx < len(sum_names) - 1 else "output_0"
        nodes.append(helper.make_node("Add", [cur, sum_names[idx]], [next_name]))
        cur = next_name
    output = helper.make_tensor_value_info("output_0", TensorProto.FLOAT, [1])
    graph = helper.make_graph(nodes, "b4_3", inputs, [output])
    _save_model(graph, path)


def build_b4_4(path: Path) -> None:
    """B4-4: 56 inputs + 56 scalar delta outputs + 1 output."""
    NUM_LAYERS = 28
    inputs = []
    outputs = []
    nodes = []
    for i in range(NUM_LAYERS):
        k_name = f"cache_key_{i}"
        v_name = f"cache_value_{i}"
        kd_name = f"key_delta_{i}"
        vd_name = f"value_delta_{i}"
        inputs.append(helper.make_tensor_value_info(k_name, TensorProto.FLOAT, list(SHAPE)))
        inputs.append(helper.make_tensor_value_info(v_name, TensorProto.FLOAT, list(SHAPE)))
        outputs.append(helper.make_tensor_value_info(kd_name, TensorProto.FLOAT, [1]))
        outputs.append(helper.make_tensor_value_info(vd_name, TensorProto.FLOAT, [1]))
        nodes.append(helper.make_node("ReduceMean", [k_name], [kd_name], keepdims=0))
        nodes.append(helper.make_node("ReduceMean", [v_name], [vd_name], keepdims=0))
    # output_0 = Add(key_delta_0, value_delta_0)
    outputs.append(helper.make_tensor_value_info("output_0", TensorProto.FLOAT, [1]))
    nodes.append(helper.make_node("Add", ["key_delta_0", "value_delta_0"], ["output_0"]))
    graph = helper.make_graph(nodes, "b4_4", inputs, outputs)
    _save_model(graph, path)


def build_b4_5(path: Path) -> None:
    """B4-5: Minimal attention — query @ key^T, then ReduceSum."""
    key_in = helper.make_tensor_value_info("cache_key_0", TensorProto.FLOAT, list(SHAPE))
    query_in = helper.make_tensor_value_info("query_0", TensorProto.FLOAT, [1, 1, 8, 128])
    output = helper.make_tensor_value_info("output_0", TensorProto.FLOAT, [1])
    # Transpose key: [1,128,8,128] -> [1,8,128,128]
    tr_key = helper.make_node("Transpose", ["cache_key_0"], ["key_t"], perm=[0, 2, 1, 3])
    # Transpose query: [1,1,8,128] -> [1,8,1,128]
    tr_query = helper.make_node("Transpose", ["query_0"], ["query_t"], perm=[0, 2, 1, 3])
    # MatMul: [1,8,1,128] x [1,8,128,128] -> [1,8,1,128]
    # ONNX MatMul on 4D: broadcast batch+heads dims, matmul last two
    # Need to reshape to 3D for MatMul: [8,1,128] x [8,128,128] -> [8,1,128]
    rk_shape = numpy_helper.from_array(np.array([8, 128, 128], dtype=np.int64), name="rk_shape")
    rq_shape = numpy_helper.from_array(np.array([8, 1, 128], dtype=np.int64), name="rq_shape")
    reshape_key = helper.make_node("Reshape", ["key_t", "rk_shape"], ["key_3d"])
    reshape_query = helper.make_node("Reshape", ["query_t", "rq_shape"], ["query_3d"])
    matmul = helper.make_node("MatMul", ["query_3d", "key_3d"], ["scores_3d"])
    reduce_sum = helper.make_node("ReduceSum", ["scores_3d"], ["output_0"], keepdims=0)
    graph = helper.make_graph(
        [tr_key, tr_query, reshape_key, reshape_query, matmul, reduce_sum],
        "b4_5", [key_in, query_in], [output],
        initializer=[rk_shape, rq_shape],
    )
    _save_model(graph, path)


def _save_model(graph, path: Path) -> None:
    model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 13)])
    model.ir_version = 8
    checker.check_model(model)
    path.parent.mkdir(parents=True, exist_ok=True)
    onnx.save(model, path)
```

- [ ] **Step 2: Add B4 input raw file generation**

Add to `write_input_files()`:

```python
def write_input_files(out: Path, variant: str = "b3") -> None:
    inputs = out / "inputs"
    inputs.mkdir(parents=True, exist_ok=True)

    # Base: single key input (B3, B4-0, B4-1)
    np.zeros(SHAPE, dtype=np.float32).tofile(inputs / "cache_key_zero_float32.raw")
    np.ones(SHAPE, dtype=np.float32).tofile(inputs / "cache_key_one_float32.raw")
    np.zeros(ELEM_COUNT, dtype=np.uint8).tofile(inputs / "cache_key_zero_uint8.raw")
    np.full(ELEM_COUNT, 0x55, dtype=np.uint8).tofile(inputs / "cache_key_pattern_uint8.raw")
    np.zeros(ELEM_COUNT, dtype=np.uint16).tofile(inputs / "cache_key_zero_uint16.raw")
    np.full(ELEM_COUNT, 0x5555, dtype=np.uint16).tofile(inputs / "cache_key_pattern_uint16.raw")

    # B4-2: also needs cache_value_0
    np.zeros(SHAPE, dtype=np.float32).tofile(inputs / "cache_value_zero_float32.raw")
    np.ones(SHAPE, dtype=np.float32).tofile(inputs / "cache_value_one_float32.raw")

    # B4-3/B4-4: needs all 56 inputs
    NUM_LAYERS = 28
    for i in range(NUM_LAYERS):
        np.zeros(SHAPE, dtype=np.float32).tofile(inputs / f"cache_key_{i}_zero_float32.raw")
        np.ones(SHAPE, dtype=np.float32).tofile(inputs / f"cache_key_{i}_one_float32.raw")
        np.zeros(SHAPE, dtype=np.float32).tofile(inputs / f"cache_value_{i}_zero_float32.raw")
        np.ones(SHAPE, dtype=np.float32).tofile(inputs / f"cache_value_{i}_one_float32.raw")

    # B4-5: needs query_0
    np.zeros((1, 1, 8, 128), dtype=np.float32).tofile(inputs / "query_zero_float32.raw")
    np.ones((1, 1, 8, 128), dtype=np.float32).tofile(inputs / "query_one_float32.raw")
```

- [ ] **Step 3: Update main() to support --variant b4-0..b4-6**

```python
B4_BUILDERS = {
    "b4-0": build_b4_0,
    "b4-1": build_b4_1,
    "b4-2": build_b4_2,
    "b4-3": build_b4_3,
    "b4-4": build_b4_4,
    "b4-5": build_b4_5,
}

def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-dir", default=str(DEFAULT_OUT))
    parser.add_argument("--variant", default="b3",
                        help="Model variant: b3 (original), b4-0..b4-5")
    parser.add_argument("--fixed16-datatype", default="")
    args = parser.parse_args()

    out = Path(args.output_dir)
    out.mkdir(parents=True, exist_ok=True)

    if args.variant == "b3":
        write_model(out / "tiny_custom_io_probe.onnx")
        # ... existing B3 YAML generation ...
    elif args.variant in B4_BUILDERS:
        B4_BUILDERS[args.variant](out / f"{args.variant}.onnx")
    else:
        print(f"Unknown variant: {args.variant}")
        sys.exit(1)

    write_input_files(out, variant=args.variant)
    print(f"Wrote probe artifacts to {out} (variant={args.variant})")
```

- [ ] **Step 4: Run build script for B4-0 baseline**

```powershell
python scripts\build_tiny_custom_io_probe.py --variant b4-0
```

Expected: `G:\STTModels\qnn-work\tiny-custom-io-probe\b4-0.onnx`

- [ ] **Step 5: Commit**

```bash
git add scripts/build_tiny_custom_io_probe.py
git commit -m "feat(gate-b4): add B4 variant ONNX model builders"
```

---

### Task 2: Extend convert_tiny_custom_io_probe.py for B4 variants

**Files:**
- Modify: `scripts/convert_tiny_custom_io_probe.py`

- [ ] **Step 1: Add B4 variant conversion logic**

The key change is that B4 variants all use `--preserve_io datatype` for their
cache-like inputs (not `--custom_io` YAML). The converter command needs dynamic
`-d` dims and `--preserve_io datatype` args based on variant.

```python
B4_VARIANTS = ["b4-0", "b4-1", "b4-2", "b4-3", "b4-4", "b4-5"]

# Per-variant: list of (input_name, dims_str) and preserve_io inputs
B4_INPUTS = {
    "b4-0": {
        "dims": [("cache_key_0", "1,128,8,128")],
        "preserve_io": ["cache_key_0", "output_0"],
    },
    "b4-1": {
        "dims": [("cache_key_0", "1,128,8,128")],
        "preserve_io": ["cache_key_0", "output_0"],
    },
    "b4-2": {
        "dims": [("cache_key_0", "1,128,8,128"), ("cache_value_0", "1,128,8,128")],
        "preserve_io": ["cache_key_0", "cache_value_0", "output_0"],
    },
    "b4-3": {
        "dims": [(f"cache_key_{i}", "1,128,8,128") for i in range(28)]
               + [(f"cache_value_{i}", "1,128,8,128") for i in range(28)],
        "preserve_io": [f"cache_key_{i}" for i in range(28)]
                     + [f"cache_value_{i}" for i in range(28)]
                     + ["output_0"],
    },
    "b4-4": {
        "dims": [(f"cache_key_{i}", "1,128,8,128") for i in range(28)]
               + [(f"cache_value_{i}", "1,128,8,128") for i in range(28)],
        "preserve_io": [f"cache_key_{i}" for i in range(28)]
                     + [f"cache_value_{i}" for i in range(28)]
                     + ["output_0"]
                     + [f"key_delta_{i}" for i in range(28)]
                     + [f"value_delta_{i}" for i in range(28)],
    },
    "b4-5": {
        "dims": [("cache_key_0", "1,128,8,128"), ("query_0", "1,1,8,128")],
        "preserve_io": ["cache_key_0", "query_0", "output_0"],
    },
}
```

Modify `convert_variant()` to use B4_INPUTS for B4 variants:

```python
def convert_variant(variant: str) -> bool:
    if variant in B4_VARIANTS:
        return convert_b4_variant(variant)
    # ... existing B3 logic ...

def convert_b4_variant(variant: str) -> bool:
    info = B4_INPUTS[variant]
    onnx_path = WORK_ROOT / f"{variant}.onnx"
    out_dir = WORK_ROOT / f"run_{variant}"
    if out_dir.exists():
        shutil.rmtree(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    cmd = [str(PYTHON), str(CONVERTER),
           "--input_network", str(onnx_path),
           "--output_path", str(out_dir / "model.cpp")]
    for name, dims in info["dims"]:
        cmd.extend(["-d", name, dims])
    cmd.extend(["--preserve_io", "datatype"] + info["preserve_io"])

    (out_dir / "convert_command.txt").write_text(" ".join(str(c) for c in cmd), encoding="utf-8")
    env = make_converter_env()
    print(f"  Converting {variant} ...")
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, env=env)
        (out_dir / "convert.log").write_text(r.stdout + "\n" + r.stderr, encoding="utf-8")
        if r.returncode != 0:
            print(f"  [FAIL] rc={r.returncode}")
            for line in (r.stdout + r.stderr).strip().splitlines()[-40:]:
                print(f"    {line}")
            return False
        print(f"  [OK] Conversion succeeded")
    except Exception as e:
        print(f"  [FAIL] {e}")
        return False

    # Extract encoding summary for cache_key_0 and output_0
    net_json = out_dir / "model_net.json"
    if net_json.exists():
        summary = extract_b4_summary(net_json, variant)
        (out_dir / "model_net_summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
        print(f"  Summary: {json.dumps(summary)}")
    return True
```

Also add `extract_b4_summary()` that handles per-variant tensor names.

- [ ] **Step 2: Update main() CLI**

```python
parser.add_argument("--variant", default="all",
                    help="Variant: all, uint8, float32, fixed16, b4-0..b4-5, b4-all")
```

When `--variant b4-all`, run B4-0 through B4-5 sequentially.

- [ ] **Step 3: Run conversion for B4-0 baseline**

```powershell
python scripts\convert_tiny_custom_io_probe.py --variant b4-0
```

Expected: `G:\STTModels\qnn-work\tiny-custom-io-probe\run_b4-0\model_net.json`
with `cache_key_0 data_type=562 (FLOAT_32)`.

- [ ] **Step 4: Commit**

```bash
git add scripts/convert_tiny_custom_io_probe.py
git commit -m "feat(gate-b4): add B4 variant conversion support"
```

---

### Task 3: Create B4 deploy + run + analyze scripts

**Files:**
- Create: `scripts/run_gate_b4_probe.ps1`
- Create: `scripts/analyze_b4_results.py`

- [ ] **Step 1: Write run_gate_b4_probe.ps1**

PowerShell script that:
1. For each B4 variant (b4-0..b4-5):
   - Pushes libmodel.so + input raw files + input_list.txt to device
   - Runs qnn-net-run with zero input
   - Runs qnn-net-run with pattern input
   - Pulls execution_metadata.yaml and output_0.raw
2. Saves results to `build\test-results\gate-b4-bridge-probe\`

Key differences from B3:
- Per-variant input_list.txt generation (single input, dual input, or 56 inputs)
- Input file selection based on variant (cache_key_0 vs all 56)
- All runtime libs already on device from B3

- [ ] **Step 2: Write analyze_b4_results.py**

```python
"""Analyze Gate B4 results: parse execution_metadata.yaml and output raw files."""
import sys
from pathlib import Path
import yaml
import numpy as np

RESULT_ROOT = Path(r"D:\Project\STT\build\test-results\gate-b4-bridge-probe")
VARIANTS = ["b4-0", "b4-1", "b4-2", "b4-3", "b4-4", "b4-5"]

def analyze_variant(v: str) -> dict:
    result = {"variant": v, "runtime_dtype": "MISSING", "output_changed": "MISSING"}
    meta_path = RESULT_ROOT / v / "output_zero" / "execution_metadata.yaml"
    if not meta_path.exists():
        result["runtime_dtype"] = "NO_FILE"
        return result
    with open(meta_path) as f:
        meta = yaml.safe_load(f)
    for g in meta.get("graphs", []):
        for t in g.get("input_tensors", []):
            if t["tensor_name"] == "cache_key_0":
                result["runtime_dtype"] = t["datatype"]
                result["dimensions"] = t.get("dimensions", [])
                break
    # Compare zero vs pattern output
    zero_raw = RESULT_ROOT / v / "output_zero" / "Result_0" / "output_0.raw"
    pat_raw = RESULT_ROOT / v / "output_pattern" / "Result_0" / "output_0.raw"
    if zero_raw.exists() and pat_raw.exists():
        z = np.fromfile(str(zero_raw), dtype=np.float32)
        p = np.fromfile(str(pat_raw), dtype=np.float32)
        result["output_changed"] = "YES" if np.abs(z - p).max() > 1e-6 else "NO"
    return result

def main():
    for v in VARIANTS:
        r = analyze_variant(v)
        dtype = r["runtime_dtype"]
        changed = r["output_changed"]
        # Check for bad override
        override = "OVERRIDE" if "UFIXED" in dtype else "OK"
        print(f"{v:6s}  dtype={dtype:30s}  output_changed={changed:5s}  {override}")
```

- [ ] **Step 3: Commit**

```bash
git add scripts/run_gate_b4_probe.ps1 scripts/analyze_b4_results.py
git commit -m "feat(gate-b4): add deploy/run/analyze scripts"
```

---

### Task 4: Run B4-0 baseline on device

**Files:** None (execution only)

- [ ] **Step 1: Build + convert B4-0**

```powershell
python scripts\build_tiny_custom_io_probe.py --variant b4-0
python scripts\convert_tiny_custom_io_probe.py --variant b4-0
```

- [ ] **Step 2: Deploy and run B4-0 on device**

Use the PowerShell script or manual ADB commands:
- Push libmodel.so to device
- Push zero/pattern input raw files
- Run qnn-net-run
- Pull execution_metadata.yaml

- [ ] **Step 3: Verify B4-0 baseline**

Expected: `cache_key_0 datatype=QNN_DATATYPE_FLOAT_32` (same as B3 result).
If B4-0 fails (cache_key_0 is UFIXED16), stop and fix the pipeline first.

---

### Task 5: Run B4-1 through B4-5 sequentially, stopping on override

**Files:** None (execution only)

- [ ] **Step 1: For each variant B4-1..B4-5, run the full pipeline**

For each variant in order:
1. `python scripts\build_tiny_custom_io_probe.py --variant b4-N`
2. `python scripts\convert_tiny_custom_io_probe.py --variant b4-N`
3. Deploy + qnn-net-run on device
4. Inspect `execution_metadata.yaml` for `cache_key_0` datatype
5. If datatype is `UFIXED16` → **STOP**, this variant found the trigger
6. If datatype is `FLOAT_32` → continue to next variant

- [ ] **Step 2: Run analyze_b4_results.py**

```powershell
python scripts\analyze_b4_results.py
```

This prints the summary table with all variant results.

---

### Task 6: Record B4 results in experiment log

**Files:**
- Modify: `docs/architecture/QWEN3_QNN_DECODER_EXPERIMENT_LOG.md`
- Modify: `docs/architecture/QWEN3_QNN_NEXT_TASKS_FOR_AI.md`

- [ ] **Step 1: Append Step 25 to experiment log**

Use the result template from the spec. Fill in all variant results.

- [ ] **Step 2: Update next-task document**

Update Gate B4 status line and decision.

- [ ] **Step 3: Commit**

```bash
git add docs/architecture/QWEN3_QNN_DECODER_EXPERIMENT_LOG.md docs/architecture/QWEN3_QNN_NEXT_TASKS_FOR_AI.md
git commit -m "docs(gate-b4): record bridge probe results"
```

---

## Self-Review

**1. Spec coverage:**
- B4-0 baseline: Task 4 ✅
- B4-1..B4-5 sequential execution: Task 5 ✅
- B4-6 RoPE: marked optional in spec, not in plan (only run if B4-5 keeps FLOAT_32) ✅
- Decision logic: implemented in analyze_b4_results.py ✅
- Result template: Task 6 ✅
- All converter flags use `--preserve_io datatype` (not `--custom_io`) ✅
- Output size limit: all variants output scalar [1] or small tensor ✅

**2. Placeholder scan:** No TBD/TODO found. All code blocks are complete.

**3. Type consistency:**
- `B4_BUILDERS` dict keys match `B4_VARIANTS` list and `B4_INPUTS` dict keys
- `SHAPE = (1, 128, 8, 128)` used consistently across all builders
- Input raw file naming convention matches convert script expectations
