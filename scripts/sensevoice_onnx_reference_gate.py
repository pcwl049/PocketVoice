import argparse
from pathlib import Path

import numpy as np
import onnxruntime as ort


def load_tokens(path: Path) -> dict[int, str]:
    tokens = {}
    for fallback_id, line in enumerate(path.read_text(encoding="utf-8").splitlines()):
        if not line.strip():
            continue
        fields = line.split()
        if len(fields) >= 2 and fields[-1].isdigit():
            tokens[int(fields[-1])] = " ".join(fields[:-1])
        else:
            tokens[fallback_id] = line.strip()
    return tokens


def decode_ctc(logits: np.ndarray, tokens: dict[int, str]) -> str:
    ids = logits.argmax(axis=-1).reshape(-1).tolist()
    ans = []
    prev = None
    for token_id in ids:
        if token_id != 0 and token_id != prev:
            ans.append(tokens.get(token_id, f"<{token_id}>"))
        prev = token_id
    return "".join(ans)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", required=True)
    parser.add_argument("--tokens", required=True)
    parser.add_argument("--features", required=True)
    parser.add_argument("--frames", type=int, default=167)
    parser.add_argument("--feature-dim", type=int, default=560)
    parser.add_argument("--expected-contains")
    args = parser.parse_args()

    features = np.fromfile(args.features, dtype=np.float32)
    expected = args.frames * args.feature_dim
    if features.size != expected:
        raise ValueError(f"Expected {expected} feature values, got {features.size}")
    x = features.reshape(1, args.frames, args.feature_dim)

    session = ort.InferenceSession(args.model, providers=["CPUExecutionProvider"])
    logits = session.run(None, {"x": x})[0]
    text = decode_ctc(logits, load_tokens(Path(args.tokens)))
    ids = logits.argmax(axis=-1).reshape(-1)
    non_blank = int(np.count_nonzero(ids != 0))

    print(f"features={x.shape} logits={logits.shape} non_blank={non_blank}/{ids.size}")
    print(f"text={text}")

    if args.expected_contains and args.expected_contains not in text:
        raise SystemExit(
            f"expected substring missing: {args.expected_contains!r}; actual: {text!r}"
        )


if __name__ == "__main__":
    main()
