#!/usr/bin/env python3
import argparse
import csv
import json
import os
import re
import sys
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def normalize_text(value: str) -> str:
    value = value.strip().lower()
    value = re.sub(r"\s+", "", value)
    value = re.sub(r"[，。！？、,.!?;:：\"'“”‘’（）()\[\]【】<>《》]", "", value)
    return value


def edit_distance(a: str, b: str) -> int:
    if a == b:
        return 0
    if not a:
        return len(b)
    if not b:
        return len(a)
    prev = list(range(len(b) + 1))
    for i, ca in enumerate(a, 1):
        cur = [i]
        for j, cb in enumerate(b, 1):
            cur.append(
                min(
                    prev[j] + 1,
                    cur[j - 1] + 1,
                    prev[j - 1] + (0 if ca == cb else 1),
                )
            )
        prev = cur
    return prev[-1]


def character_error_rate(expected: str, actual: str) -> float:
    expected = normalize_text(expected)
    actual = normalize_text(actual)
    if not expected:
        return 0.0 if not actual else 1.0
    return edit_distance(expected, actual) / len(expected)


def split_hotwords(raw: str) -> list[str]:
    if not raw:
        return []
    return [item.strip() for item in re.split(r"[,，;；]", raw) if item.strip()]


def read_eval_manifest(path: Path) -> list[dict]:
    rows = []
    with path.open("r", encoding="utf-8-sig", newline="") as f:
        reader = csv.DictReader(
            (line for line in f if line.strip() and not line.lstrip().startswith("#")),
            delimiter="\t",
        )
        required = {"file"}
        missing = required - set(reader.fieldnames or [])
        if missing:
            raise ValueError(f"{path} missing columns: {', '.join(sorted(missing))}")
        seen = set()
        for row in reader:
            if row.get("mode", "wav") != "wav":
                continue
            wav = row["file"].strip()
            if not wav or wav in seen:
                continue
            seen.add(wav)
            expected = row.get("expected_text") or row.get("expected_contains") or ""
            rows.append(
                {
                    "uttid": Path(wav).stem,
                    "file": wav,
                    "expected": expected,
                    "hotwords": split_hotwords(row.get("hotwords", "")),
                }
            )
    return rows


def load_model(model_dir: Path, threads: int):
    if not model_dir.exists():
        raise RuntimeError(
            f"Paraformer contextual ONNX model not found: {model_dir}\n"
            "Download it with:\n"
            "  modelscope download --model damo/speech_paraformer-large-contextual_asr_nat-zh-cn-16k-common-vocab8404-onnx --local_dir models/paraformer-contextual-onnx"
        )
    try:
        from funasr_onnx import ContextualParaformer
    except Exception as exc:
        raise RuntimeError(
            "Could not import funasr_onnx. Install it in this Python environment:\n"
            "  python -m pip install funasr-onnx jieba"
        ) from exc
    eb_file = model_dir / "model_eb.onnx"
    eb_quant_file = model_dir / "model_eb_quant.onnx"
    if eb_file.exists() and not eb_quant_file.exists():
        try:
            os.link(eb_file, eb_quant_file)
        except OSError:
            import shutil

            shutil.copy2(eb_file, eb_quant_file)
    model = ContextualParaformer(model_dir=str(model_dir), quantize=True, intra_op_num_threads=threads)
    if not hasattr(model, "language"):
        model.language = "zh"
    return model


def format_hotwords(words: list[str], extra_words: list[str]) -> str:
    merged = []
    seen = set()
    for word in words + extra_words:
        clean = word.strip()
        if not clean or clean in seen:
            continue
        seen.add(clean)
        merged.append(clean)
    return " ".join(merged) if merged else "<s>"


def result_text(result: list[dict]) -> str:
    if not result:
        return ""
    preds = result[0].get("preds", "")
    if isinstance(preds, str):
        return preds
    if isinstance(preds, (tuple, list)) and preds:
        return str(preds[0])
    return str(preds)


def evaluate(args) -> int:
    manifest = read_eval_manifest(args.manifest)
    if not manifest:
        raise RuntimeError(f"No wav rows found in {args.manifest}")
    rows = manifest[: args.limit] if args.limit else manifest

    model = load_model(args.model, args.threads)
    extra_hotwords = split_hotwords(args.extra_hotwords)
    args.out.parent.mkdir(parents=True, exist_ok=True)

    summary_rows = []
    started_all = time.perf_counter()
    with args.out.open("w", encoding="utf-8", newline="\n") as f:
        for row in rows:
            wav_path = (ROOT / row["file"]).resolve()
            hotwords = format_hotwords(row["hotwords"], extra_hotwords)
            started = time.perf_counter()
            result = model(str(wav_path), hotwords=hotwords)
            elapsed_ms = int((time.perf_counter() - started) * 1000)
            text = result_text(result)
            cer = character_error_rate(row["expected"], text)
            hits = [word for word in row["hotwords"] if normalize_text(word) in normalize_text(text)]
            item = {
                "uttid": row["uttid"],
                "file": row["file"],
                "expected": row["expected"],
                "actual": text,
                "cer": cer,
                "hotwords": row["hotwords"],
                "hotword_hits": hits,
                "hotword_prompt": hotwords,
                "elapsed_ms": elapsed_ms,
                "raw": result,
            }
            summary_rows.append(item)
            f.write(json.dumps(item, ensure_ascii=False) + "\n")

    elapsed_ms = int((time.perf_counter() - started_all) * 1000)
    avg_cer = sum(item["cer"] for item in summary_rows) / len(summary_rows)
    total_hotwords = sum(len(item["hotwords"]) for item in summary_rows)
    hit_hotwords = sum(len(item["hotword_hits"]) for item in summary_rows)
    print(f"Paraformer contextual rows: {len(summary_rows)}")
    print(f"Elapsed: {elapsed_ms} ms")
    print(f"Average CER: {avg_cer:.4f}")
    if total_hotwords:
        print(f"Hotword hits: {hit_hotwords}/{total_hotwords}")
    print(f"JSONL: {args.out}")
    return 0


def self_test() -> int:
    assert normalize_text(" DeepSeek，破防了！") == "deepseek破防了"
    assert character_error_rate("你好世界", "你好世界") == 0
    assert character_error_rate("你好世界", "你好") == 0.5
    assert split_hotwords("DeepSeek, QNN；哈基米") == ["DeepSeek", "QNN", "哈基米"]
    assert format_hotwords(["QNN", "QNN"], ["HTP"]) == "QNN HTP"
    assert result_text([{"preds": ("你好", ["你", "好"])}]) == "你好"
    print("paraformer_contextual_eval self-test passed")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Evaluate Paraformer contextual ONNX on PocketVoice wav manifests.")
    parser.add_argument("--model", type=Path, default=Path(os.environ.get("PARAFORMER_CONTEXTUAL_MODEL", ROOT / "models" / "paraformer-contextual-onnx")))
    parser.add_argument("--manifest", type=Path, default=ROOT / "test_audio" / "expected.tsv")
    parser.add_argument("--out", type=Path, default=ROOT / "build" / "test-results" / "paraformer-contextual-eval.jsonl")
    parser.add_argument("--threads", type=int, default=int(os.environ.get("PARAFORMER_CONTEXTUAL_THREADS", "4")))
    parser.add_argument("--extra-hotwords", default=os.environ.get("PARAFORMER_CONTEXTUAL_EXTRA_HOTWORDS", ""))
    parser.add_argument("--limit", type=int, default=0)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()

    if args.self_test:
        return self_test()
    return evaluate(args)


if __name__ == "__main__":
    raise SystemExit(main())
