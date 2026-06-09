import argparse
from pathlib import Path

import numpy as np
import onnx
from onnx import numpy_helper


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--prompt", default="0,1,2,14")
    parser.add_argument("--expand-feature-broadcasts", action="store_true")
    parser.add_argument("--frames", type=int, default=167)
    args = parser.parse_args()

    prompt = np.array([int(item) for item in args.prompt.split(",")], dtype=np.int32)
    if prompt.shape != (4,):
        raise ValueError(f"SenseVoice prompt must have 4 values, got {prompt.shape}")

    model = onnx.load(args.input)
    inputs = [value for value in model.graph.input if value.name != "prompt"]
    del model.graph.input[:]
    model.graph.input.extend(inputs)
    model.graph.initializer.append(numpy_helper.from_array(prompt, name="prompt"))

    if args.expand_feature_broadcasts:
        for node in model.graph.node:
            if node.op_type != "Constant":
                continue
            for attr in node.attribute:
                if attr.name != "value" or not attr.HasField("t"):
                    continue
                value = numpy_helper.to_array(attr.t)
                if value.shape == (1, 1, 560):
                    expanded = np.repeat(value, args.frames, axis=1).astype(value.dtype)
                    attr.t.CopyFrom(numpy_helper.from_array(expanded))

    model = onnx.shape_inference.infer_shapes(model)

    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    onnx.save(model, output)


if __name__ == "__main__":
    main()
