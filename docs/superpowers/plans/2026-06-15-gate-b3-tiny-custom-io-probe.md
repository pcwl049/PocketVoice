# Gate B3: Tiny Custom IO QuantParam Probe — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build and run a tiny ONNX model on the device to test whether `--custom_io` QuantParam can affect HTP APP_WRITE input runtime encoding after `graphFinalize`.

**Architecture:** Three phases — (1) Python scripts build a tiny ONNX model and convert it with custom_io YAML variants, (2) C++ probe code in the Android APK loads the tiny model, audits finalized encoding, and tests input/output responsiveness, (3) a batch script deploys and runs the probe via logcat.

**Tech Stack:** Python + ONNX for model construction, QAIRT QNN converter for ONNX→QNN, NDK clang++ for libmodel.so, C++ QNN API for runtime probe, ADB for deployment.

---

## File Structure

| File | Action | Responsibility |
|------|--------|----------------|
| `scripts/build_tiny_custom_io_probe.py` | Create | Build tiny ONNX model + generate custom_io YAML variants |
| `scripts/convert_tiny_custom_io_probe.py` | Create | Convert tiny ONNX → QNN model.cpp + build libmodel.so for each custom_io variant |
| `src/mobile/app/src/main/cpp/qwen3_qnn_backend.h` | Modify | Add `runCustomIoProbe()` declaration |
| `src/mobile/app/src/main/cpp/qwen3_qnn_backend.cpp` | Modify | Implement `runCustomIoProbe()` — standalone QNN init/finalize/audit/execute |
| `src/mobile/app/src/main/cpp/stt_engine.cpp` | Modify | Add env-var-gated `runCustomIoProbe()` call |
| `scripts/test_qnn_custom_io_probe.bat` | Create | Deploy tiny model files + trigger probe via ADB |

---

### Task 1: Build Tiny ONNX Model + Generate custom_io YAML

**Files:**
- Create: `scripts/build_tiny_custom_io_probe.py`

- [ ] **Step 1: Write the build script**

```python
"""Build tiny ONNX probe model + custom_io YAML variants for Gate B3."""
import argparse
from pathlib import Path

import numpy as np
from onnx import helper, TensorProto, numpy_helper


def build_probe_onnx(output_path: Path):
    """Build a tiny ONNX model: cache_key_0 [1,128,8,128] -> Add(1.0) -> ReduceSum -> output_0 [1]."""
    cache_in = helper.make_tensor_value_info(
        "cache_key_0", TensorProto.FLOAT, [1, 128, 8, 128]
    )
    output_0 = helper.make_tensor_value_info(
        "output_0", TensorProto.FLOAT, [1]
    )

    # Constant 1.0 scalar for Add
    one_const = numpy_helper.from_array(
        np.array(1.0, dtype=np.float32), name="one_const"
    )

    add_node = helper.make_node(
        "Add", inputs=["cache_key_0", "one_const"], outputs=["added"]
    )
    reduce_node = helper.make_node(
        "ReduceSum", inputs=["added"], outputs=["output_0"], keepdims=1
    )

    graph = helper.make_graph(
        [add_node, reduce_node],
        "tiny_custom_io_probe",
        [cache_in],
        [output_0],
        initializer=[one_const],
    )
    model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 13)])
    model.ir_version = 8

    from onnx import checker
    checker.check_model(model)

    output_path.parent.mkdir(parents=True, exist_ok=True)
    with open(output_path, "wb") as f:
        f.write(model.SerializeToString())
    print(f"Wrote ONNX model: {output_path} ({output_path.stat().st_size} bytes)")


def write_custom_io_yaml(path: Path, io_list: list):
    """Write a custom_io YAML file for QNN converter."""
    lines = []
    for io in io_list:
        lines.append(f"- IOName: {io['name']}")
        if "layout" in io:
            lines.append("  Layout:")
            lines.append(f"    Model: {io['layout']}")
            lines.append(f"    Custom: {io['layout']}")
        if "datatype" in io:
            lines.append(f"  Datatype: {io['datatype']}")
        if "quantparam" in io:
            qp = io["quantparam"]
            lines.append("  QuantParam:")
            lines.append(f"    Type: {qp['type']}")
            lines.append(f"    Scale: {qp['scale']}")
            lines.append(f"    Offset: {qp['offset']}")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines), encoding="utf-8")
    print(f"Wrote custom_io YAML: {path}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--output-dir",
        default=r"G:\STTModels\qnn-work\tiny-custom-io-probe",
    )
    args = parser.parse_args()
    out = Path(args.output_dir)

    build_probe_onnx(out / "tiny_custom_io_probe.onnx")

    # Run 1: uint8 with QuantParam (control — QuantParam should work for uint8)
    write_custom_io_yaml(
        out / "custom_io_uint8.yaml",
        [
            {
                "name": "cache_key_0",
                "layout": "NCHW",
                "datatype": "uint8",
                "quantparam": {
                    "type": "QNN_DEFINITION_DEFINED",
                    "scale": 0.015625,
                    "offset": 0,
                },
            },
            {
                "name": "output_0",
                "layout": "F",
                "datatype": "uint8",
                "quantparam": {
                    "type": "QNN_DEFINITION_DEFINED",
                    "scale": 0.015625,
                    "offset": 0,
                },
            },
        ],
    )

    # Run 2: float32 with QuantParam (QuantParam expected to be ignored)
    write_custom_io_yaml(
        out / "custom_io_float32.yaml",
        [
            {
                "name": "cache_key_0",
                "layout": "NCHW",
                "datatype": "float32",
                "quantparam": {
                    "type": "QNN_DEFINITION_DEFINED",
                    "scale": 0.015625,
                    "offset": 0,
                },
            },
            {
                "name": "output_0",
                "layout": "F",
                "datatype": "float32",
            },
        ],
    )

    print("Done. Next: run convert_tiny_custom_io_probe.py")


if __name__ == "__main__":
    main()
```

- [ ] **Step 2: Run the build script**

Run: `python scripts/build_tiny_custom_io_probe.py`

Expected: Three files created in `G:\STTModels\qnn-work\tiny-custom-io-probe\`:
- `tiny_custom_io_probe.onnx`
- `custom_io_uint8.yaml`
- `custom_io_float32.yaml`

- [ ] **Step 3: Commit**

```bash
git add scripts/build_tiny_custom_io_probe.py
git commit -m "feat(gate-b3): add tiny ONNX probe model builder + custom_io YAML generator"
```

---

### Task 2: Convert Tiny ONNX → QNN + Build libmodel.so

**Files:**
- Create: `scripts/convert_tiny_custom_io_probe.py`

- [ ] **Step 1: Write the convert + build script**

```python
"""Convert tiny ONNX probe to QNN model.cpp and build libmodel.so for each custom_io variant."""
import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path


ROOT = Path(r"D:\Project\STT")
QAIRT = Path(r"G:\Program Files\qairt\2.45.0.260326")
PYTHON = ROOT / "build" / "qairt-py310-venv" / "Scripts" / "python.exe"
CONVERTER = QAIRT / "bin" / "x86_64-windows-msvc" / "qnn-onnx-converter"
WORK_ROOT = Path(r"G:\STTModels\qnn-work\tiny-custom-io-probe")
ONNX_MODEL = WORK_ROOT / "tiny_custom_io_probe.onnx"

VARIANTS = ["uint8", "float32"]

# NDK / build paths (mirrors build_qnn_model_lib_android_decoder_fullkv.bat)
NDK = Path(r"D:\Project\STT\third_party\android-ndk-r27c")
TOOLBIN = NDK / "toolchains" / "llvm" / "prebuilt" / "windows-x86_64" / "bin"
CLANG = TOOLBIN / "aarch64-linux-android21-clang++.cmd"
OBJCOPY = TOOLBIN / "llvm-objcopy.exe"
QNN_INCLUDE = QAIRT / "include" / "QNN"
QNN_JNI = QAIRT / "share" / "QNN" / "converter" / "jni"


def convert_variant(variant: str, yaml_path: Path, out_dir: Path, log_path: Path):
    """Run QNN converter for one variant."""
    if out_dir.exists():
        shutil.rmtree(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    cmd = [
        str(PYTHON), str(CONVERTER),
        "--input_network", str(ONNX_MODEL),
        "--output_path", str(out_dir / "model.cpp"),
        "-d", "cache_key_0", "1,128,8,128",
        "--custom_io", str(yaml_path),
    ]
    env = os.environ.copy()
    env["QNN_SDK_ROOT"] = str(QAIRT)
    env["QNN_SDK"] = str(QAIRT)
    env["PYTHONPATH"] = str(QAIRT / "lib" / "python")
    env["QAIRT_TMP_DIR"] = str(WORK_ROOT / "tmp")

    print(f"[{variant}] Converting: {' '.join(cmd)}")
    with log_path.open("w", encoding="utf-8") as log:
        result = subprocess.run(cmd, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=300)
    if result.returncode != 0:
        tail = log_path.read_text(encoding="utf-8", errors="replace").splitlines()[-40:]
        print(f"[{variant}] CONVERSION FAILED. Last 40 lines:", file=sys.stderr)
        for line in tail:
            print(line, file=sys.stderr)
        return False

    # Check model_net.json
    model_json = out_dir / "model_net.json"
    if model_json.exists():
        data = json.loads(model_json.read_text(encoding="utf-8"))
        tensors = data.get("graph", {}).get("tensors", {})
        for name in ["cache_key_0", "output_0"]:
            if name in tensors:
                t = tensors[name]
                qp = t.get("quant_params", {})
                so = qp.get("scale_offset", {})
                print(f"  {name}: dtype={t.get('data_type')} scale={so.get('scale')} "
                      f"offset={so.get('offset')} is_overridden={qp.get('is_overridden')}")
    return True


def build_libmodel(convert_dir: Path, out_dir: Path):
    """Build libmodel.so from converted model.cpp + model.bin."""
    jni = out_dir / "jni"
    obj_root = out_dir / "manual-obj"
    raw_dir = out_dir / "obj" / "binary"
    libs_dir = out_dir / "libs" / "arm64-v8a"
    local_include = out_dir / "qnn-include"

    for d in [jni, obj_root, raw_dir, libs_dir, local_include]:
        d.mkdir(parents=True, exist_ok=True)

    # Copy JNI template files
    for item in QNN_JNI.iterdir():
        if item.is_file():
            shutil.copy2(item, jni / item.name)
        elif item.is_dir():
            shutil.copytree(item, jni / item.name, dirs_exist_ok=True)

    # Copy model files
    shutil.copy2(convert_dir / "model.cpp", jni / "model.cpp")
    shutil.copy2(convert_dir / "model.bin", jni / "model.bin")

    # Copy QNN include
    for item in QNN_INCLUDE.iterdir():
        if item.is_file():
            shutil.copy2(item, local_include / item.name)
        elif item.is_dir():
            shutil.copytree(item, local_include / item.name, dirs_exist_ok=True)

    # Extract raw files from model.bin
    subprocess.run(
        ["tar", "-xf", str(jni / "model.bin")],
        cwd=str(raw_dir), check=True, capture_output=True,
    )
    raw_files = list(raw_dir.glob("*.raw"))
    print(f"  Extracted {len(raw_files)} raw weight files")

    # objcopy each .raw to .o
    for raw_f in raw_files:
        obj_f = obj_root / (raw_f.stem + ".o")
        subprocess.run(
            [str(OBJCOPY), "-I", "binary", "-O", "elf64-littleaarch64",
             "-B", "aarch64", str(raw_f), str(obj_f)],
            check=True, capture_output=True,
        )

    # Compile C++ sources
    compile_flags = [
        "-std=c++11", "-O3", "-fPIC", "-fvisibility=hidden",
        f"-DQNN_API=__attribute__((visibility(\"default\")))",
        f"-I{jni}", f"-I{local_include}", "-Wno-write-strings",
    ]
    cpp_sources = ["QnnModel.cpp", "QnnWrapperUtils.cpp", "linux/QnnModelPal.cpp", "model.cpp"]
    obj_names = ["QnnModel.cpp.o", "QnnWrapperUtils.cpp.o", "linux_QnnModelPal.cpp.o", "model.cpp.o"]
    for src, obj_name in zip(cpp_sources, obj_names):
        subprocess.run(
            [str(CLANG), "-c", str(jni / src), "-o", str(obj_root / obj_name)] + compile_flags,
            check=True, capture_output=True,
        )

    # Link all .o files into libmodel.so
    rsp_path = obj_root / "objects.rsp"
    all_objs = list(obj_root.glob("*.o"))
    rsp_path.write_text("\n".join(f'"{o}"' for o in all_objs), encoding="utf-8")

    subprocess.run(
        [str(CLANG), "-shared", "-o", str(libs_dir / "libmodel.so"),
         f"@{rsp_path}", "-Wl,-z,max-page-size=16384", "-lc", "-lm", "-ldl"],
        check=True, capture_output=True,
    )

    so_size = (libs_dir / "libmodel.so").stat().st_size
    print(f"  libmodel.so: {so_size} bytes")
    return True


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--variant", choices=VARIANTS + ["all"], default="all")
    args = parser.parse_args()

    variants = VARIANTS if args.variant == "all" else [args.variant]

    for v in variants:
        yaml_path = WORK_ROOT / f"custom_io_{v}.yaml"
        convert_dir = WORK_ROOT / f"run_{v}"
        log_path = WORK_ROOT / f"convert_{v}.log"
        build_dir = WORK_ROOT / f"build_{v}"

        print(f"\n=== Variant: {v} ===")
        if not yaml_path.exists():
            print(f"  SKIP: {yaml_path} not found. Run build_tiny_custom_io_probe.py first.")
            continue

        if not convert_variant(v, yaml_path, convert_dir, log_path):
            print(f"  Conversion failed for {v}, skipping build.")
            continue

        build_libmodel(convert_dir, build_dir)

    print("\nDone. libmodel.so files are in G:\\STTModels\\qnn-work\\tiny-custom-io-probe\\build_<variant>\\libs\\arm64-v8a\\")


if __name__ == "__main__":
    main()
```

- [ ] **Step 2: Run the convert + build script**

Run: `python scripts/convert_tiny_custom_io_probe.py --variant all`

Expected: Two directories created:
- `G:\STTModels\qnn-work\tiny-custom-io-probe\build_uint8\libs\arm64-v8a\libmodel.so`
- `G:\STTModels\qnn-work\tiny-custom-io-probe\build_float32\libs\arm64-v8a\libmodel.so`

If uint8 conversion fails (converter may reject uint8 for a float32 ONNX input), record the error and continue with float32 variant only.

- [ ] **Step 3: Verify model_net.json encodings**

For each variant, read `G:\STTModels\qnn-work\tiny-custom-io-probe\run_<variant>\model_net.json` and confirm `cache_key_0` has the expected `data_type` and `quant_params` matching the custom_io YAML.

- [ ] **Step 4: Commit**

```bash
git add scripts/convert_tiny_custom_io_probe.py
git commit -m "feat(gate-b3): add tiny ONNX probe converter + libmodel.so builder"
```

---

### Task 3: Implement C++ `runCustomIoProbe()` — Header

**Files:**
- Modify: `src/mobile/app/src/main/cpp/qwen3_qnn_backend.h`

- [ ] **Step 1: Add `runCustomIoProbe()` declaration**

Find the line with `bool runKvInfluenceProbe();` in `qwen3_qnn_backend.h` and add after it:

```cpp
    bool runCustomIoProbe(const std::string& modelDir);
```

The final method list in the public section should include:

```cpp
    bool runKvInfluenceProbe();
    bool runCustomIoProbe(const std::string& modelDir);
    bool auditRuntimeEncodings();
```

- [ ] **Step 2: Commit**

```bash
git add src/mobile/app/src/main/cpp/qwen3_qnn_backend.h
git commit -m "feat(gate-b3): add runCustomIoProbe() declaration"
```

---

### Task 4: Implement C++ `runCustomIoProbe()` — Implementation

**Files:**
- Modify: `src/mobile/app/src/main/cpp/qwen3_qnn_backend.cpp`

This is the core probe. It creates its own isolated QNN backend/device/context for the tiny model, separate from the main decoder. It audits finalized tensor encoding and tests whether different inputs produce different outputs.

- [ ] **Step 1: Add `runCustomIoProbe()` implementation**

Add this method at the end of the `Qwen3QnnBackend` class (before the closing of the file, after `dumpDiagRecords()`). It uses `dlopen`/`dlsym` to load the tiny model's `libmodel.so`, creates a standalone QNN session, and runs the encoding audit + input responsiveness test.

```cpp
bool Qwen3QnnBackend::runCustomIoProbe(const std::string& modelDir) {
    LOGI("=== Gate B3: Custom IO Probe Start (modelDir=%s) ===", modelDir.c_str());

    // ---- 1. Load tiny model libmodel.so ----
    std::string modelSoPath = modelDir + "/libmodel.so";
    void* modelLib = dlopen(modelSoPath.c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (!modelLib) {
        LOGE("[GateB3] dlopen failed: %s", dlerror());
        return false;
    }
    LOGI("[GateB3] Loaded model: %s", modelSoPath.c_str());

    // ---- 2. Get model compose function ----
    using ComposeGraphsFn = int(*)(Qnn_ModelHandle_t, const QnnInterface_t*, Qnn_ContextHandle_t);
    auto composeFn = reinterpret_cast<ComposeGraphsFn>(dlsym(modelLib, "QnnModel_composeGraphs"));
    if (!composeFn) {
        LOGE("[GateB3] dlsym QnnModel_composeGraphs failed: %s", dlerror());
        dlclose(modelLib);
        return false;
    }

    // ---- 3. Get QNN HTP backend interface ----
    std::string htpSoPath = modelDir + "/libQnnHtp.so";
    // Fallback: try the main app's lib directory
    if (access(htpSoPath.c_str(), F_OK) != 0) {
        htpSoPath = modelDir + "/../libQnnHtp.so";
    }
    void* htpLib = dlopen(htpSoPath.c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (!htpLib) {
        LOGE("[GateB3] dlopen HTP failed: %s", dlerror());
        dlclose(modelLib);
        return false;
    }

    auto getProvidersFn = reinterpret_cast<QnnInterface_getProvidersFn_t>(
        dlsym(htpLib, "QnnInterface_getProviders"));
    if (!getProvidersFn) {
        LOGE("[GateB3] dlsym QnnInterface_getProviders failed");
        dlclose(htpLib);
        dlclose(modelLib);
        return false;
    }

    QnnInterface_t* qnnInterfaceHandle = nullptr;
    uint32_t numProviders = 0;
    Qnn_ErrorHandle_t err = getProvidersFn(&qnnInterfaceHandle, &numProviders);
    if (err != QNN_SUCCESS || numProviders == 0) {
        LOGE("[GateB3] QnnInterface_getProviders failed: %lu", (unsigned long)err);
        dlclose(htpLib);
        dlclose(modelLib);
        return false;
    }
    QnnInterface_t qnnInterface = qnnInterfaceHandle[0];
    LOGI("[GateB3] QNN interface obtained: backend=%u provider=%s",
         qnnInterface.providerId, qnnInterface.providerName ? qnnInterface.providerName : "?");

    // ---- 4. Create backend, device, context ----
    Qnn_LogHandle_t logHandle = nullptr;
    Qnn_BackendHandle_t backendHandle = nullptr;
    Qnn_DeviceHandle_t deviceHandle = nullptr;
    Qnn_ContextHandle_t contextHandle = nullptr;

    err = qnnInterface.backendCreate(logHandle, nullptr, &backendHandle);
    if (err != QNN_SUCCESS) {
        LOGE("[GateB3] backendCreate failed: %lu", (unsigned long)err);
        goto cleanup;
    }

    err = qnnInterface.deviceCreate(logHandle, nullptr, &deviceHandle);
    if (err != QNN_SUCCESS) {
        LOGE("[GateB3] deviceCreate failed: %lu", (unsigned long)err);
        goto cleanup;
    }

    err = qnnInterface.contextCreate(backendHandle, deviceHandle, nullptr, &contextHandle);
    if (err != QNN_SUCCESS) {
        LOGE("[GateB3] contextCreate failed: %lu", (unsigned long)err);
        goto cleanup;
    }

    // ---- 5. Compose graphs ----
    {
        Qnn_ModelHandle_t modelHandle = nullptr;
        int rc = composeFn(modelHandle, &qnnInterface, contextHandle);
        if (rc != 0) {
            LOGE("[GateB3] composeGraphs failed: %d", rc);
            goto cleanup;
        }
    }

    // ---- 6. Retrieve graph info and finalize ----
    {
        Qnn_GraphHandle_t graph = nullptr;
        uint32_t numGraphs = 0;
        err = qnnInterface.graphRetrieve(contextHandle, &graph, &numGraphs);
        if (err != QNN_SUCCESS || numGraphs == 0) {
            LOGE("[GateB3] graphRetrieve failed: %lu numGraphs=%u",
                 (unsigned long)err, numGraphs);
            goto cleanup;
        }

        err = qnnInterface.graphFinalize(graph, nullptr, nullptr);
        if (err != QNN_SUCCESS) {
            LOGE("[GateB3] graphFinalize failed: %lu", (unsigned long)err);
            goto cleanup;
        }
        LOGI("[GateB3] graphFinalize succeeded");

        // ---- 7. Get graph info (input/output tensors) ----
        Qnn_GraphInfo_t graphInfo;
        memset(&graphInfo, 0, sizeof(graphInfo));
        graphInfo.version = QNN_GRAPH_INFO_VERSION_1;
        err = qnnInterface.graphGetInfo(graph, &graphInfo);
        if (err != QNN_SUCCESS) {
            LOGE("[GateB3] graphGetInfo failed: %lu", (unsigned long)err);
            goto cleanup;
        }

        // For QNN API version compatibility, we use the same approach as the main decoder:
        // read input/output tensor info from the GraphInfo_t stored during composeGraphs.
        // However, for a standalone probe we need to access the graph's tensors directly.
        // Use graph API to enumerate input/output tensors.

        // ---- 7a. Audit input tensor encoding ----
        LOGI("[GateB3] --- Input tensor encoding audit ---");
        for (uint32_t i = 0; i < graphInfo.numInputTensors; i++) {
            Impl::logTensorEncoding("GateB3", graphInfo.inputTensors[i]);
        }

        LOGI("[GateB3] --- Output tensor encoding audit ---");
        for (uint32_t i = 0; i < graphInfo.numOutputTensors; i++) {
            Impl::logTensorEncoding("GateB3", graphInfo.outputTensors[i]);
        }

        // ---- 8. Check if cache_key_0 has usable encoding ----
        const Qnn_Tensor_t* inputTensor = nullptr;
        const Qnn_Tensor_t* outputTensor = nullptr;
        for (uint32_t i = 0; i < graphInfo.numInputTensors; i++) {
            const char* name = Impl::tensorName(graphInfo.inputTensors[i]);
            if (name && strcmp(name, "cache_key_0") == 0) {
                inputTensor = &graphInfo.inputTensors[i];
            }
        }
        for (uint32_t i = 0; i < graphInfo.numOutputTensors; i++) {
            const char* name = Impl::tensorName(graphInfo.outputTensors[i]);
            if (name && strcmp(name, "output_0") == 0) {
                outputTensor = &graphInfo.outputTensors[i];
            }
        }

        if (!inputTensor || !outputTensor) {
            LOGE("[GateB3] Could not find cache_key_0 or output_0 in graph tensors");
            goto cleanup;
        }

        // Log the critical encoding info
        {
            const auto dataType = Impl::tensorDataType(*inputTensor);
            const auto& qp = Impl::tensorQuantParams(*inputTensor);
            float inputScale = 0.0f;
            int32_t inputOffset = 0;
            if (qp.encodingDefinition == QNN_DEFINITION_DEFINED &&
                qp.quantizationEncoding == QNN_QUANTIZATION_ENCODING_SCALE_OFFSET) {
                inputScale = qp.scaleOffsetEncoding.scale;
                inputOffset = qp.scaleOffsetEncoding.offset;
            }
            LOGI("[GateB3] cache_key_0: dtype=%d scale=%.10e offset=%d",
                 (int)dataType, inputScale, inputOffset);

            // Verdict: is the encoding usable?
            bool usable = false;
            if (dataType == QNN_DATATYPE_FLOAT_32 || dataType == QNN_DATATYPE_FLOAT_16) {
                usable = true;
                LOGI("[GateB3] VERDICT: Float dtype — bypasses bad quantization scale");
            } else if (dataType == QNN_DATATYPE_UFIXED_POINT_16 && inputScale > 1e-6f) {
                usable = true;
                LOGI("[GateB3] VERDICT: Fixed16 with usable scale=%.6e (range covers key_absmax~330)",
                     inputScale);
            } else {
                LOGI("[GateB3] VERDICT: Unusable encoding — scale=%.10e too small for KV data",
                     inputScale);
            }
        }

        // ---- 9. Prepare two input buffers and execute ----
        const size_t elemCount = Impl::tensorElementCount(*inputTensor);
        const size_t inputBufSize = elemCount * Impl::tensorElementSize(*inputTensor);
        const size_t outputElemCount = Impl::tensorElementCount(*outputTensor);
        const size_t outputBufSize = outputElemCount * Impl::tensorElementSize(*outputTensor);

        std::vector<uint8_t> inputBufA(inputBufSize, 0);
        std::vector<uint8_t> inputBufB(inputBufSize, 0);

        // Fill Buffer B based on runtime dtype
        const auto inputDtype = Impl::tensorDataType(*inputTensor);
        if (inputDtype == QNN_DATATYPE_FLOAT_32) {
            auto* f = reinterpret_cast<float*>(inputBufB.data());
            for (size_t i = 0; i < elemCount; ++i) f[i] = 1.0f;
        } else if (inputDtype == QNN_DATATYPE_UFIXED_POINT_16 ||
                   inputDtype == QNN_DATATYPE_SFIXED_POINT_16 ||
                   inputDtype == QNN_DATATYPE_FLOAT_16) {
            auto* u16 = reinterpret_cast<uint16_t*>(inputBufB.data());
            for (size_t i = 0; i < elemCount; ++i) u16[i] = 0x5555;
        } else if (inputDtype == QNN_DATATYPE_UINT_8) {
            std::fill(inputBufB.begin(), inputBufB.end(), 0x55);
        } else {
            // Default: fill with non-zero byte pattern
            std::fill(inputBufB.begin(), inputBufB.end(), 0x55);
        }

        // Prepare input/output tensor structs for execution
        Qnn_Tensor_t execInput = *inputTensor;
        Qnn_Tensor_t execOutput = *outputTensor;

        auto setClientBuf = [](Qnn_Tensor_t& t, void* data, uint32_t size) {
            if (t.version == QNN_TENSOR_VERSION_2) {
                t.v2.memType = QNN_TENSORMEMTYPE_RAW;
                t.v2.clientBuf.data = data;
                t.v2.clientBuf.dataSize = size;
            } else {
                t.v1.memType = QNN_TENSORMEMTYPE_RAW;
                t.v1.clientBuf.data = data;
                t.v1.clientBuf.dataSize = size;
            }
        };

        std::vector<uint8_t> outputBufA(outputBufSize, 0);
        std::vector<uint8_t> outputBufB(outputBufSize, 0);

        // Run A: zero input
        setClientBuf(execInput, inputBufA.data(), static_cast<uint32_t>(inputBufSize));
        setClientBuf(execOutput, outputBufA.data(), static_cast<uint32_t>(outputBufSize));
        err = qnnInterface.graphExecute(graph, &execInput, 1, &execOutput, 1, nullptr, nullptr);
        if (err != QNN_SUCCESS) {
            LOGE("[GateB3] graphExecute Run A failed: %lu", (unsigned long)err);
            goto cleanup;
        }

        // Run B: non-zero input
        setClientBuf(execInput, inputBufB.data(), static_cast<uint32_t>(inputBufSize));
        setClientBuf(execOutput, outputBufB.data(), static_cast<uint32_t>(outputBufSize));
        err = qnnInterface.graphExecute(graph, &execInput, 1, &execOutput, 1, nullptr, nullptr);
        if (err != QNN_SUCCESS) {
            LOGE("[GateB3] graphExecute Run B failed: %lu", (unsigned long)err);
            goto cleanup;
        }

        // ---- 10. Compare outputs ----
        {
            float outA = 0.0f, outB = 0.0f;
            Impl::readTensorToFloat(*outputTensor, outputBufA.data(), &outA, 1);
            Impl::readTensorToFloat(*outputTensor, outputBufB.data(), &outB, 1);
            const float diff = std::abs(outA - outB);
            const bool outputChanges = diff > 1e-6f;

            LOGI("[GateB3] Run A output=%.6f", outA);
            LOGI("[GateB3] Run B output=%.6f", outB);
            LOGI("[GateB3] diff=%.6f", diff);
            LOGI("[GateB3] Output changes with input: %s", outputChanges ? "YES" : "NO");
        }
    }

    LOGI("=== Gate B3: Custom IO Probe Complete ===");

cleanup:
    if (contextHandle) qnnInterface.contextFree(contextHandle, nullptr);
    if (deviceHandle) qnnInterface.deviceFree(deviceHandle, nullptr);
    if (backendHandle) qnnInterface.backendFree(backendHandle, nullptr);
    dlclose(htpLib);
    dlclose(modelLib);
    return true;
}
```

**Important implementation note:** The QNN `graphGetInfo` API may not be available or may have version compatibility issues. If `graphGetInfo` fails, an alternative approach is to use the `QnnModel_getGraphsInfo` function exported by the model library itself (same as how the main decoder uses `graphsInfo`). The engineer should check which approach works by examining how `qwen3_qnn_backend.cpp` retrieves graph info in its `init()` method and mirror that pattern.

Specifically, in the main decoder's `init()` (around line 1036), the code uses:
```cpp
QnnModel_getGraphsInfo(&gi, &numGraphs);
```

This function is exported by `libmodel.so`. The probe should use the same approach:
```cpp
using GetGraphsInfoFn = Qnn_ErrorHandle_t(*)(const Qnn_GraphInfo_t**, uint32_t*);
auto getGraphsInfoFn = reinterpret_cast<GetGraphsInfoFn>(dlsym(modelLib, "QnnModel_getGraphsInfo"));
```

If the `graphGetInfo` API fails, replace that section with `QnnModel_getGraphsInfo`.

- [ ] **Step 2: Build the Android APK to verify compilation**

Run: `scripts\build_mobile_apk.bat`

Expected: APK builds without errors. The new `runCustomIoProbe()` method compiles cleanly.

- [ ] **Step 3: Commit**

```bash
git add src/mobile/app/src/main/cpp/qwen3_qnn_backend.cpp
git commit -m "feat(gate-b3): implement runCustomIoProbe() — standalone QNN encoding audit + input test"
```

---

### Task 5: Integrate Probe Call into stt_engine.cpp

**Files:**
- Modify: `src/mobile/app/src/main/cpp/stt_engine.cpp`

- [ ] **Step 1: Add env-var-gated probe call**

Find the block after the KV influence probe call (around line 717-721) and add after it:

```cpp
            // Gate B3: Custom IO QuantParam Probe
            // Triggered by system property or environment variable
            const char* gateB3Dir = getenv("STT_GATE_B3_PROBE");
            if (gateB3Dir) {
                LOGI("Running Gate B3 custom IO probe from: %s", gateB3Dir);
                m_impl->qwen3QnnBackend->runCustomIoProbe(gateB3Dir);
            }
```

This ensures the probe only runs when explicitly requested via environment variable, not in normal operation.

Note: Android apps cannot read host environment variables directly. The alternative is to use a system property set via `adb shell setprop`. However, `getenv()` works if the property is passed through the JNI bridge. A simpler approach for testing is to hardcode the probe directory temporarily or pass it via an intent extra. The engineer should check how the existing codebase handles such debug flags and follow the same pattern.

If `getenv()` does not work on Android for this case, use `__system_property_get`:
```cpp
#include <sys/system_properties.h>
// ...
char gateB3Dir[256] = {};
__system_property_get("stt.gate_b3.dir", gateB3Dir);
if (gateB3Dir[0] != '\0') {
    LOGI("Running Gate B3 custom IO probe from: %s", gateB3Dir);
    m_impl->qwen3QnnBackend->runCustomIoProbe(gateB3Dir);
}
```

- [ ] **Step 2: Build APK**

Run: `scripts\build_mobile_apk.bat`

Expected: APK builds without errors.

- [ ] **Step 3: Commit**

```bash
git add src/mobile/app/src/main/cpp/stt_engine.cpp
git commit -m "feat(gate-b3): add env-var-gated runCustomIoProbe() call in stt_engine"
```

---

### Task 6: Create Deploy + Run Script

**Files:**
- Create: `scripts/test_qnn_custom_io_probe.bat`

- [ ] **Step 1: Write the deploy + run script**

```bat
@echo off
setlocal EnableExtensions

call "%~dp0env.bat"

set "PROBE_ROOT=G:\STTModels\qnn-work\tiny-custom-io-probe"
set "DEVICE_DIR=/data/local/tmp/tiny-custom-io-probe"
set "ADB=%ADB%"
if "%ADB%"=="" set "ADB=adb"

echo === Gate B3: Deploy + Run Custom IO Probe ===

echo.
echo [1/3] Pushing probe files to device...
%ADB% shell mkdir -p %DEVICE_DIR%/run_uint8
%ADB% shell mkdir -p %DEVICE_DIR%/run_float32

if exist "%PROBE_ROOT%\build_uint8\libs\arm64-v8a\libmodel.so" (
    %ADB% push "%PROBE_ROOT%\build_uint8\libs\arm64-v8a\libmodel.so" %DEVICE_DIR%/run_uint8/
    echo   uint8 libmodel.so pushed
) else (
    echo   [SKIP] uint8 libmodel.so not found
)

if exist "%PROBE_ROOT%\build_float32\libs\arm64-v8a\libmodel.so" (
    %ADB% push "%PROBE_ROOT%\build_float32\libs\arm64-v8a\libmodel.so" %DEVICE_DIR%/run_float32/
    echo   float32 libmodel.so pushed
) else (
    echo   [SKIP] float32 libmodel.so not found
)

echo.
echo [2/3] Setting system property for probe trigger...
%ADB% shell setprop stt.gate_b3.dir %DEVICE_DIR%/run_uint8

echo.
echo [3/3] Launching app and capturing logcat...
echo Look for [GateB3] tags in the output.
echo Press Ctrl+C to stop.
echo.
%ADB% logcat -c
%ADB% logcat -s GateB3:*
```

- [ ] **Step 2: Test the deployment flow**

Run: `scripts\test_qnn_custom_io_probe.bat`

Expected: libmodel.so files pushed to device, app launched, logcat shows Gate B3 probe output with encoding audit and input/output comparison.

- [ ] **Step 3: Commit**

```bash
git add scripts/test_qnn_custom_io_probe.bat
git commit -m "feat(gate-b3): add deploy + run script for custom IO probe"
```

---

### Task 7: Run Probe + Record Results

This task is manual — requires device interaction.

- [ ] **Step 1: Run the full pipeline**

```bat
REM Step 1: Build tiny ONNX + custom_io YAML
python scripts/build_tiny_custom_io_probe.py

REM Step 2: Convert + build libmodel.so
python scripts/convert_tiny_custom_io_probe.py --variant all

REM Step 3: Build APK
scripts\build_mobile_apk.bat

REM Step 4: Deploy + run
scripts\test_qnn_custom_io_probe.bat
```

- [ ] **Step 2: Record results in experiment log**

Add results to `docs/architecture/QWEN3_QNN_DECODER_EXPERIMENT_LOG.md` as the next step. Record:
- For each variant (uint8, float32):
  - Converter output `model_net.json` encoding
  - HTP runtime encoding after finalize (dtype, scale, offset)
  - Whether output changes between zero and non-zero input
- Verdict: PASS or FAIL for Gate B3
- Decision: proceed to Gate C if FAIL, or consider full decoder custom_io conversion if PASS

- [ ] **Step 3: Update task document**

Update `docs/architecture/QWEN3_QNN_NEXT_TASKS_FOR_AI.md` with Gate B3 result.

- [ ] **Step 4: Commit**

```bash
git add docs/architecture/QWEN3_QNN_DECODER_EXPERIMENT_LOG.md docs/architecture/QWEN3_QNN_NEXT_TASKS_FOR_AI.md
git commit -m "docs: record Gate B3 custom_io QuantParam probe results"
```

---

## Self-Review

**1. Spec coverage:**
- Phase 1 (Build + Convert): Task 1 + Task 2 ✓
- Phase 2 (C++ probe code): Task 3 + Task 4 ✓
- Phase 3 (Integration): Task 5 + Task 6 ✓
- Results recording: Task 7 ✓
- All acceptance criteria from spec are testable through Task 4 Step 10 (encoding audit + output comparison)

**2. Placeholder scan:**
- No TBD/TODO found
- All code steps have complete implementation
- All commands are explicit

**3. Type consistency:**
- `runCustomIoProbe(const std::string& modelDir)` declared in Task 3, used in Task 5 with matching signature
- `Impl::logTensorEncoding()`, `Impl::tensorName()`, `Impl::tensorElementCount()`, `Impl::tensorElementSize()`, `Impl::tensorDataType()`, `Impl::tensorQuantParams()`, `Impl::readTensorToFloat()` all exist in the current codebase (verified via subagent exploration)
- `QnnInterface_getProvidersFn_t` type is already defined in the existing code at line 848 of `qwen3_qnn_backend.cpp`
