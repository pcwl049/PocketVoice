#!/usr/bin/env python3
"""Gate B3 Task 2: Convert tiny custom_io probe ONNX model with QNN converter
and build Android arm64-v8a libmodel.so for each variant.

Usage:
    python scripts/convert_tiny_custom_io_probe.py --variant all
    python scripts/convert_tiny_custom_io_probe.py --variant uint8
    python scripts/convert_tiny_custom_io_probe.py --variant float32
    python scripts/convert_tiny_custom_io_probe.py --variant fixed16
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import tarfile
from pathlib import Path

# ── Paths ──────────────────────────────────────────────────────────────────
ROOT = Path(r"D:\Project\STT")
QAIRT = Path(r"G:\Program Files\qairt\2.45.0.260326")
PYTHON = ROOT / "build" / "qairt-py310-venv" / "Scripts" / "python.exe"
CONVERTER = QAIRT / "bin" / "x86_64-windows-msvc" / "qnn-onnx-converter"
WORK_ROOT = Path(r"G:\STTModels\qnn-work\tiny-custom-io-probe")
ONNX_MODEL = WORK_ROOT / "tiny_custom_io_probe.onnx"
NDK = ROOT / "third_party" / "android-ndk-r27c"
TOOLBIN = NDK / "toolchains" / "llvm" / "prebuilt" / "windows-x86_64" / "bin"
CLANG = TOOLBIN / "aarch64-linux-android21-clang++.cmd"
OBJCOPY = TOOLBIN / "llvm-objcopy.exe"
QNN_INCLUDE = QAIRT / "include" / "QNN"
QNN_JNI = QAIRT / "share" / "QNN" / "converter" / "jni"

VARIANTS = ["uint8", "float32", "fixed16"]


def run(cmd, cwd=None, env=None):
    """Run a command, return CompletedProcess. Raises on error."""
    r = subprocess.run(cmd, capture_output=True, text=True, cwd=cwd, env=env)
    if r.returncode != 0:
        raise RuntimeError(
            f"Command failed (rc={r.returncode}): {' '.join(str(c) for c in cmd)}\n"
            f"stdout: {r.stdout[-2000:]}\nstderr: {r.stderr[-2000:]}"
        )
    return r


def make_converter_env():
    """Build environment dict for QNN converter with required PYTHONPATH."""
    env = os.environ.copy()
    env["QNN_SDK_ROOT"] = str(QAIRT)
    env["QNN_SDK"] = str(QAIRT)
    env["PYTHONPATH"] = str(QAIRT / "lib" / "python")
    tmp_dir = WORK_ROOT / "tmp"
    tmp_dir.mkdir(parents=True, exist_ok=True)
    env["QAIRT_TMP_DIR"] = str(tmp_dir)
    return env


def extract_summary(net_json_path: Path) -> dict:
    """Extract encoding info for cache_key_0 and output_0 from model_net.json."""
    with open(net_json_path, "r", encoding="utf-8") as f:
        net = json.load(f)

    # model_net.json may have tensors as a top-level list or under graph.tensors
    tensors = {}
    if "tensors" in net:
        t_list = net["tensors"]
        if isinstance(t_list, list):
            for t in t_list:
                tensors[t.get("name")] = t
        elif isinstance(t_list, dict):
            tensors = t_list
    if "graph" in net and "tensors" in net["graph"]:
        t_list = net["graph"]["tensors"]
        if isinstance(t_list, list):
            for t in t_list:
                tensors[t.get("name")] = t
        elif isinstance(t_list, dict):
            tensors.update(t_list)

    summary = {}
    for tensor_name in ["cache_key_0", "output_0"]:
        t = tensors.get(tensor_name, {})
        qp = t.get("quant_params", {})
        so = qp.get("scale_offset", {})
        summary[tensor_name] = {
            "data_type": t.get("data_type"),
            "scale": so.get("scale"),
            "offset": so.get("offset"),
            "is_overridden": qp.get("is_overridden"),
        }
    return summary


def convert_variant(variant: str) -> bool:
    """Run QNN ONNX converter for one variant. Returns True on success."""
    out_dir = WORK_ROOT / f"run_{variant}"
    out_dir.mkdir(parents=True, exist_ok=True)

    # Build converter command based on variant type
    cmd = [
        str(PYTHON), str(CONVERTER),
        "--input_network", str(ONNX_MODEL),
        "--output_path", str(out_dir / "model.cpp"),
        "-d", "cache_key_0", "1,128,8,128",
    ]

    if variant == "float32":
        # --custom_io does NOT accept float32 as a quantized datatype.
        # Use --preserve_io datatype to keep float32 IO.
        cmd.extend(["--preserve_io", "datatype", "cache_key_0", "output_0"])
    elif variant == "fixed16":
        yaml_path = WORK_ROOT / "custom_io_fixed16.yaml"
        if not yaml_path.exists():
            print(f"  [SKIP] custom_io_fixed16.yaml not found, skipping fixed16")
            return False
        cmd.extend(["--custom_io", str(yaml_path)])
    else:
        # uint8 and any other variants use --custom_io YAML
        yaml_path = WORK_ROOT / f"custom_io_{variant}.yaml"
        if not yaml_path.exists():
            print(f"  [SKIP] {yaml_path.name} not found, skipping {variant}")
            return False
        cmd.extend(["--custom_io", str(yaml_path)])

    # Save command text for reproducibility
    (out_dir / "convert_command.txt").write_text(
        " ".join(str(c) for c in cmd) + "\n", encoding="utf-8"
    )

    env = make_converter_env()
    log_path = out_dir / "convert.log"

    print(f"  Converting {variant} ...")
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, env=env)
        log_text = r.stdout + "\n" + r.stderr
        log_path.write_text(log_text, encoding="utf-8")
        if r.returncode != 0:
            lines = log_text.strip().splitlines()
            print(f"  [FAIL] Converter returned rc={r.returncode}")
            for line in lines[-80:]:
                print(f"    {line}")
            return False
        print(f"  [OK] Conversion succeeded")
    except Exception as e:
        print(f"  [FAIL] Exception: {e}")
        return False

    # Extract encoding summary from model_net.json
    net_json = out_dir / "model_net.json"
    if net_json.exists():
        try:
            summary = extract_summary(net_json)
            summary_path = out_dir / "model_net_summary.json"
            summary_path.write_text(json.dumps(summary, indent=2), encoding="utf-8")
            print(f"  Encoding summary: {json.dumps(summary)}")
        except Exception as e:
            print(f"  [WARN] Could not extract summary: {e}")

    return True


def build_libmodel(variant: str) -> bool:
    """Build libmodel.so for one variant. Returns True on success."""
    run_dir = WORK_ROOT / f"run_{variant}"
    build_dir = WORK_ROOT / f"build_{variant}"

    model_cpp = run_dir / "model.cpp"
    model_bin = run_dir / "model.bin"
    if not model_cpp.exists():
        print(f"  [SKIP] model.cpp missing for {variant}")
        return False

    # model.bin is optional (tiny models with no weights may not have it)
    has_model_bin = model_bin.exists()

    # Clean and create build directory structure
    if build_dir.exists():
        shutil.rmtree(build_dir)
    jni_dir = build_dir / "jni"
    obj_dir = build_dir / "obj"
    raw_dir = build_dir / "obj" / "binary"
    qnn_inc = build_dir / "qnn-include"
    libs_dir = build_dir / "libs" / "arm64-v8a"
    for d in [jni_dir, obj_dir, raw_dir, qnn_inc, libs_dir]:
        d.mkdir(parents=True, exist_ok=True)

    # Copy JNI template files
    shutil.copytree(str(QNN_JNI), str(jni_dir), dirs_exist_ok=True)
    # Copy model.cpp (overwrite the template if any)
    shutil.copy2(str(model_cpp), str(jni_dir / "model.cpp"))
    # Copy QNN include
    shutil.copytree(str(QNN_INCLUDE), str(qnn_inc), dirs_exist_ok=True)

    raw_files = []
    if has_model_bin:
        shutil.copy2(str(model_bin), str(jni_dir / "model.bin"))
        # Extract raw weight files from model.bin
        print(f"  Extracting model.bin ...")
        try:
            with tarfile.open(str(model_bin), "r") as tf:
                tf.extractall(path=str(raw_dir))
        except Exception as e:
            print(f"  [FAIL] tar extract: {e}")
            return False

        raw_files = list(raw_dir.glob("*.raw"))
        print(f"  Extracted {len(raw_files)} raw files")

    # objcopy each .raw -> .o
    # IMPORTANT: Use relative path from raw_dir so symbol names are
    # _binary_<name>_raw_start (not _binary_<full_path>_<name>_raw_start).
    # BINVARSTART/BINLEN macros expect _binary_obj_binary_<name>_raw_start,
    # so we also rename symbols to match.
    print(f"  Running objcopy ...")
    for raw_f in raw_files:
        o_file = obj_dir / (raw_f.stem + ".o")
        # Run objcopy with cwd=raw_dir so the path in symbol names is short
        # Then rename the symbols to match what BINVARSTART expects:
        #   _binary_<filename>_raw_start -> _binary_obj_binary_<filename>_raw_start
        base_sym = f"_binary_{raw_f.stem}_raw"
        target_sym = f"_binary_obj_binary_{raw_f.stem}_raw"
        try:
            run([
                str(OBJCOPY),
                "-I", "binary",
                "-O", "elf64-littleaarch64",
                "-B", "aarch64",
                raw_f.name,      # relative path from cwd
                str(o_file),
            ], cwd=str(raw_dir))
            # Rename symbols: _binary_<name>_raw_start -> _binary_obj_binary_<name>_raw_start
            run([
                str(OBJCOPY),
                "--redefine-sym", f"{base_sym}_start={target_sym}_start",
                "--redefine-sym", f"{base_sym}_end={target_sym}_end",
                "--redefine-sym", f"{base_sym}_size={target_sym}_size",
                str(o_file),
            ])
        except RuntimeError as e:
            print(f"  [FAIL] objcopy {raw_f.name}: {e}")
            return False

    # Compile C++ sources
    print(f"  Compiling C++ sources ...")
    compile_sources = [
        ("QnnModel.cpp", "QnnModel.cpp.o"),
        ("QnnWrapperUtils.cpp", "QnnWrapperUtils.cpp.o"),
        ("linux/QnnModelPal.cpp", "linux_QnnModelPal.cpp.o"),
        ("model.cpp", "model.cpp.o"),
    ]
    compile_flags = [
        "-std=c++11", "-O3", "-fPIC",
        "-fvisibility=hidden",
        "-DQNN_API=__attribute__((visibility(\"default\")))",
        f"-I{jni_dir}",
        f"-I{qnn_inc}",
        "-Wno-write-strings",
    ]
    for src_name, obj_name in compile_sources:
        src_path = jni_dir / src_name
        obj_path = obj_dir / obj_name
        try:
            run([str(CLANG), "-c", str(src_path), "-o", str(obj_path)] + compile_flags)
        except RuntimeError as e:
            print(f"  [FAIL] compile {src_name}: {e}")
            return False

    # Link libmodel.so
    print(f"  Linking libmodel.so ...")
    # Use forward slashes in response file to avoid backslash-escape issues with clang on Windows
    all_objs = list(obj_dir.glob("*.o"))
    rsp_path = obj_dir / "objects.rsp"
    rsp_path.write_text("\n".join(str(o).replace("\\", "/") for o in all_objs), encoding="utf-8")

    so_path = libs_dir / "libmodel.so"
    try:
        run([
            str(CLANG),
            "-shared",
            "-o", str(so_path),
            f"@{rsp_path}",
            "-Wl,-z,max-page-size=16384",
            "-lc", "-lm", "-ldl",
        ])
    except RuntimeError as e:
        print(f"  [FAIL] link: {e}")
        return False

    so_size = so_path.stat().st_size
    print(f"  [OK] libmodel.so: {so_size:,} bytes")
    return True


def main():
    parser = argparse.ArgumentParser(description="Gate B3 Task 2: Convert + Build tiny custom_io probe")
    parser.add_argument("--variant", default="all",
                        help="Variant to process: all, uint8, float32, fixed16")
    args = parser.parse_args()

    if args.variant == "all":
        variants = VARIANTS
    else:
        variants = [args.variant]

    # Pre-flight checks
    if not ONNX_MODEL.exists():
        print(f"[ERROR] ONNX model not found: {ONNX_MODEL}")
        sys.exit(1)
    if not PYTHON.exists():
        print(f"[ERROR] Python venv not found: {PYTHON}")
        sys.exit(1)
    if not CONVERTER.exists():
        print(f"[ERROR] QNN converter not found: {CONVERTER}")
        sys.exit(1)
    if not CLANG.exists():
        print(f"[ERROR] NDK clang not found: {CLANG}")
        sys.exit(1)

    results = {}
    for v in variants:
        print(f"\n{'='*60}")
        print(f"Variant: {v}")
        print(f"{'='*60}")

        convert_ok = convert_variant(v)
        build_ok = False
        if convert_ok:
            build_ok = build_libmodel(v)

        results[v] = {"convert": convert_ok, "build": build_ok}
        status = "OK" if (convert_ok and build_ok) else ("CONVERT_FAIL" if not convert_ok else "BUILD_FAIL")
        print(f"  Result: {status}")

    # Summary
    print(f"\n{'='*60}")
    print("Summary")
    print(f"{'='*60}")
    for v, r in results.items():
        c = "OK" if r["convert"] else ("SKIP" if v == "fixed16" else "FAIL")
        b = "OK" if r["build"] else ("SKIP" if not r["convert"] else "FAIL")
        print(f"  {v:10s}  convert={c}  build={b}")

    # Exit with error if any variant that was attempted failed
    failed = [v for v, r in results.items() if r["convert"] and not r["build"]]
    if failed:
        print(f"\n[ERROR] Build failed for: {', '.join(failed)}")
        sys.exit(1)

    convert_failed = [v for v, r in results.items() if not r["convert"] and v != "fixed16"]
    if convert_failed and len(convert_failed) == len([v for v in variants if v != "fixed16"]):
        print(f"\n[ERROR] All non-fixed16 variants failed conversion")
        sys.exit(1)

    print("\nDone.")


if __name__ == "__main__":
    main()
