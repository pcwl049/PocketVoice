import argparse
import json
import os
import shutil
import subprocess
from pathlib import Path


ROOT = Path(r"D:\Project\STT")
QAIRT = Path(r"G:\Program Files\qairt\2.45.0.260326")
PYTHON = ROOT / "build" / "qairt-py310-venv" / "Scripts" / "python.exe"
CONVERTER = QAIRT / "bin" / "x86_64-windows-msvc" / "qnn-onnx-converter"
MODEL = Path(r"G:\STTModels\models\Qwen3-ASR-onnx\model_0.6B\decoder_single_model_const_kv.onnx")
INPUT_LIST = Path(r"G:\STTModels\qnn-work\qwen3-decoder-calib-with-kv\single_model\input_list.txt")
OUT_ROOT = Path(r"G:\STTModels\qnn-work\qnn-convert")

BATCH = 1
SEQ = 1
N_AUDIO_TOKENS = 65
MAX_TOTAL_LEN = 128
NUM_HEADS = 8
HEAD_DIM = 128
NUM_LAYERS = 28


def input_dims():
    dims = [
        "-d", "input_ids", f"{BATCH},{SEQ}",
        "-d", "audio_features", f"{BATCH},{N_AUDIO_TOKENS},1024",
        "-d", "attention_mask", f"{BATCH},{SEQ}",
    ]
    for i in range(NUM_LAYERS):
        dims.extend(["-d", f"cache_key_{i}", f"{BATCH},{MAX_TOTAL_LEN},{NUM_HEADS},{HEAD_DIM}"])
        dims.extend(["-d", f"cache_value_{i}", f"{BATCH},{MAX_TOTAL_LEN},{NUM_HEADS},{HEAD_DIM}"])
    dims.extend(["-d", "rope_emb", f"{SEQ},64"])
    dims.extend(["-d", "attention_bias", f"{SEQ},{SEQ}"])
    dims.extend(["-d", "position_scalar", "1"])
    return dims


def input_dtype_overrides():
    return [
        "--input_dtype", "input_ids", "int32",
        "--input_dtype", "attention_mask", "int32",
        "--input_dtype", "position_scalar", "int32",
    ]


def preserve_io_datatype_args():
    """Return --preserve_io datatype args for cache_key/cache_value tensors.
    This forces the converter to declare these inputs as float32 APP_WRITE,
    bypassing the HTP quantization scale override problem."""
    args = ["--preserve_io", "datatype"]
    for i in range(NUM_LAYERS):
        args.append(f"cache_key_{i}")
        args.append(f"cache_value_{i}")
    return args


def write_kv_override(path: Path, kv_absmax: float, bitwidth: int):
    enc = {
        "bitwidth": bitwidth,
        "dtype": "int",
        "min": -float(kv_absmax),
        "max": float(kv_absmax),
        "is_symmetric": "False",
    }
    activation_encodings = {}
    for i in range(NUM_LAYERS):
        activation_encodings[f"cache_key_{i}"] = [dict(enc)]
        activation_encodings[f"cache_value_{i}"] = [dict(enc)]
    data = {
        "activation_encodings": activation_encodings,
        "param_encodings": {},
        "version": "0.5.0",
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2), encoding="ascii")


def inspect_cache_encodings(model_json: Path):
    data = json.loads(model_json.read_text(encoding="utf-8"))
    out = {}
    for name in ["cache_key_0", "cache_value_0", "cache_key_27", "cache_value_27"]:
        tensor = data["graph"]["tensors"][name]
        qp = tensor["quant_params"]
        so = qp["scale_offset"]
        out[name] = {
            "bitwidth": so["bitwidth"],
            "min": so["minimum"],
            "max": so["maximum"],
            "scale": so["scale"],
            "offset": so["offset"],
            "is_overridden": qp.get("is_overridden"),
        }
    return out


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--variant", default="qwen3-decoder-fullkv-act16-single-model-f32kv-i32")
    parser.add_argument("--kv-absmax", type=float, default=512.0)
    parser.add_argument("--bitwidth", type=int, default=16)
    parser.add_argument("--timeout", type=int, default=3600)
    args = parser.parse_args()

    required = [QAIRT, PYTHON, CONVERTER, MODEL, INPUT_LIST]
    missing = [str(p) for p in required if not p.exists()]
    if missing:
        raise SystemExit("Missing required paths:\n" + "\n".join(missing))

    out_dir = OUT_ROOT / args.variant
    log_file = OUT_ROOT / f"{args.variant}.convert.log"
    override_file = OUT_ROOT / f"{args.variant}.quantization_overrides.json"

    if out_dir.exists():
        shutil.rmtree(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    write_kv_override(override_file, args.kv_absmax, args.bitwidth)

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

    env = os.environ.copy()
    env["QNN_SDK_ROOT"] = str(QAIRT)
    env["QNN_SDK"] = str(QAIRT)
    env["PYTHONPATH"] = str(QAIRT / "lib" / "python")
    env["QAIRT_TMP_DIR"] = r"G:\STTModels\qnn-work\tmp"

    print(f"variant={args.variant}")
    print(f"override={override_file}")
    print(f"output={out_dir}")
    print(f"log={log_file}")

    with log_file.open("w", encoding="utf-8") as log:
        result = subprocess.run(cmd, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=args.timeout)

    if result.returncode != 0:
        tail = log_file.read_text(encoding="utf-8", errors="replace").splitlines()[-80:]
        print("\n".join(tail))
        raise SystemExit(result.returncode)

    model_json = out_dir / "model_net.json"
    if not model_json.exists():
        raise SystemExit(f"Missing conversion output: {model_json}")

    print(json.dumps(inspect_cache_encodings(model_json), indent=2))


if __name__ == "__main__":
    main()
