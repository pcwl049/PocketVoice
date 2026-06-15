"""Build tiny ONNX + custom_io YAML + input raw files for Gate B3."""
from __future__ import annotations

import argparse
from pathlib import Path

import numpy as np
import onnx
from onnx import TensorProto, checker, helper, numpy_helper


DEFAULT_OUT = Path(r"G:\STTModels\qnn-work\tiny-custom-io-probe")
SHAPE = (1, 128, 8, 128)
ELEM_COUNT = int(np.prod(SHAPE))


def write_model(path: Path) -> None:
    cache_in = helper.make_tensor_value_info("cache_key_0", TensorProto.FLOAT, list(SHAPE))
    output = helper.make_tensor_value_info("output_0", TensorProto.FLOAT, [1])

    one = numpy_helper.from_array(np.array(1.0, dtype=np.float32), name="one_const")
    add = helper.make_node("Add", ["cache_key_0", "one_const"], ["added"])
    reduce_sum = helper.make_node("ReduceSum", ["added"], ["output_0"], keepdims=0)

    graph = helper.make_graph(
        [add, reduce_sum],
        "tiny_custom_io_probe",
        [cache_in],
        [output],
        initializer=[one],
    )
    model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 13)])
    model.ir_version = 8
    checker.check_model(model)
    path.parent.mkdir(parents=True, exist_ok=True)
    onnx.save(model, path)


def write_yaml(path: Path, entries: list[dict]) -> None:
    lines: list[str] = []
    for entry in entries:
        lines.append(f"- IOName: {entry['name']}")
        if entry.get("layout"):
            lines.append("  Layout:")
            lines.append(f"    Model: {entry['layout']}")
            lines.append(f"    Custom: {entry['layout']}")
        if entry.get("datatype"):
            lines.append(f"  Datatype: {entry['datatype']}")
        if entry.get("quant"):
            q = entry["quant"]
            lines.append("  QuantParam:")
            lines.append("    Type: QNN_DEFINITION_DEFINED")
            lines.append(f"    Scale: {q['scale']}")
            lines.append(f"    Offset: {q['offset']}")
    path.write_text("\n".join(lines) + "\n", encoding="ascii")


def write_input_files(out: Path) -> None:
    inputs = out / "inputs"
    inputs.mkdir(parents=True, exist_ok=True)

    np.zeros(SHAPE, dtype=np.float32).tofile(inputs / "cache_key_zero_float32.raw")
    np.ones(SHAPE, dtype=np.float32).tofile(inputs / "cache_key_one_float32.raw")

    np.zeros(ELEM_COUNT, dtype=np.uint8).tofile(inputs / "cache_key_zero_uint8.raw")
    np.full(ELEM_COUNT, 0x55, dtype=np.uint8).tofile(inputs / "cache_key_pattern_uint8.raw")

    np.zeros(ELEM_COUNT, dtype=np.uint16).tofile(inputs / "cache_key_zero_uint16.raw")
    np.full(ELEM_COUNT, 0x5555, dtype=np.uint16).tofile(inputs / "cache_key_pattern_uint16.raw")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-dir", default=str(DEFAULT_OUT))
    parser.add_argument(
        "--fixed16-datatype",
        default="",
        help="Documented custom_io datatype spelling for 16-bit fixed input. Leave empty to skip.",
    )
    args = parser.parse_args()

    out = Path(args.output_dir)
    out.mkdir(parents=True, exist_ok=True)

    write_model(out / "tiny_custom_io_probe.onnx")

    write_yaml(
        out / "custom_io_uint8.yaml",
        [
            {"name": "cache_key_0", "layout": "NCHW", "datatype": "uint8", "quant": {"scale": 0.015625, "offset": 0}},
            {"name": "output_0", "layout": "F", "datatype": "uint8", "quant": {"scale": 0.015625, "offset": 0}},
        ],
    )

    write_yaml(
        out / "custom_io_float32.yaml",
        [
            {"name": "cache_key_0", "layout": "NCHW", "datatype": "float32", "quant": {"scale": 0.015625, "offset": 0}},
            {"name": "output_0", "layout": "F", "datatype": "float32"},
        ],
    )

    if args.fixed16_datatype:
        write_yaml(
            out / "custom_io_fixed16.yaml",
            [
                {
                    "name": "cache_key_0",
                    "layout": "NCHW",
                    "datatype": args.fixed16_datatype,
                    "quant": {"scale": 0.015625, "offset": 0},
                },
                {"name": "output_0", "layout": "F", "datatype": args.fixed16_datatype},
            ],
        )

    write_input_files(out)
    print(f"Wrote Gate B3 probe artifacts to {out}")
    print("Note: uint8 is a control only. It is not an acceptance path for full decoder KV.")


if __name__ == "__main__":
    main()
