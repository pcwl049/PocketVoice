#!/usr/bin/env python3
import argparse
import csv
import json
import re
import time
from pathlib import Path

import numpy as np
import soundfile as sf


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
            cur.append(min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (0 if ca == cb else 1)))
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
        if "file" not in (reader.fieldnames or []):
            raise ValueError(f"{path} missing column: file")
        seen = set()
        for row in reader:
            if row.get("mode", "wav") != "wav":
                continue
            wav = row["file"].strip()
            if not wav or wav in seen:
                continue
            seen.add(wav)
            rows.append(
                {
                    "uttid": Path(wav).stem,
                    "file": wav,
                    "expected": row.get("expected_text") or row.get("expected_contains") or "",
                    "hotwords": split_hotwords(row.get("hotwords", "")),
                }
            )
    return rows


def load_wave(path: Path) -> tuple[int, np.ndarray]:
    samples, sample_rate = sf.read(path, dtype="float32", always_2d=False)
    if samples.ndim == 2:
        samples = samples.mean(axis=1)
    return sample_rate, np.ascontiguousarray(samples)


def require_path(path: str, label: str) -> str:
    if not path:
        raise ValueError(f"--{label} is required")
    if not Path(path).exists():
        raise FileNotFoundError(f"{label} not found: {path}")
    return path


def make_recognizer(args):
    import sherpa_onnx

    kind = args.kind
    common = {"num_threads": args.threads, "debug": args.debug, "provider": args.provider}
    if kind == "zipformer_ctc":
        return sherpa_onnx.OfflineRecognizer.from_zipformer_ctc(
            model=require_path(args.model, "model"), tokens=require_path(args.tokens, "tokens"), **common
        )
    if kind == "sense_voice":
        return sherpa_onnx.OfflineRecognizer.from_sense_voice(
            model=require_path(args.model, "model"),
            tokens=require_path(args.tokens, "tokens"),
            language=args.language,
            use_itn=args.use_itn,
            **common,
        )
    if kind == "paraformer":
        return sherpa_onnx.OfflineRecognizer.from_paraformer(
            paraformer=require_path(args.model, "model"), tokens=require_path(args.tokens, "tokens"), **common
        )
    if kind == "firered_ctc":
        return sherpa_onnx.OfflineRecognizer.from_fire_red_asr_ctc(
            model=require_path(args.model, "model"), tokens=require_path(args.tokens, "tokens"), **common
        )
    if kind == "firered_aed":
        return sherpa_onnx.OfflineRecognizer.from_fire_red_asr(
            encoder=require_path(args.encoder, "encoder"),
            decoder=require_path(args.decoder, "decoder"),
            tokens=require_path(args.tokens, "tokens"),
            **common,
        )
    if kind == "whisper":
        return sherpa_onnx.OfflineRecognizer.from_whisper(
            encoder=require_path(args.encoder, "encoder"),
            decoder=require_path(args.decoder, "decoder"),
            tokens=require_path(args.tokens, "tokens"),
            language=args.language or "zh",
            task="transcribe",
            tail_paddings=args.tail_paddings,
            **common,
        )
    if kind == "qwen3_asr":
        return sherpa_onnx.OfflineRecognizer.from_qwen3_asr(
            conv_frontend=require_path(args.conv_frontend, "conv-frontend"),
            encoder=require_path(args.encoder, "encoder"),
            decoder=require_path(args.decoder, "decoder"),
            tokenizer=require_path(args.tokenizer, "tokenizer"),
            hotwords=args.extra_hotwords,
            max_total_len=args.max_total_len,
            max_new_tokens=args.max_new_tokens,
            **common,
        )
    if kind == "funasr_nano":
        return sherpa_onnx.OfflineRecognizer.from_funasr_nano(
            encoder_adaptor=require_path(args.encoder_adaptor, "encoder-adaptor"),
            llm=require_path(args.llm, "llm"),
            embedding=require_path(args.embedding, "embedding"),
            tokenizer=require_path(args.tokenizer, "tokenizer"),
            hotwords=args.extra_hotwords,
            language=args.language,
            max_new_tokens=args.max_new_tokens,
            **common,
        )
    raise ValueError(f"Unsupported kind: {kind}")


def decode_one(recognizer, wav_path: Path) -> str:
    sample_rate, samples = load_wave(wav_path)
    stream = recognizer.create_stream()
    stream.accept_waveform(sample_rate, samples)
    recognizer.decode_stream(stream)
    return stream.result.text


def evaluate(args) -> int:
    rows = read_eval_manifest(args.manifest)
    if args.limit:
        rows = rows[: args.limit]
    if not rows:
        raise RuntimeError(f"No wav rows found in {args.manifest}")

    recognizer = make_recognizer(args)
    args.out.parent.mkdir(parents=True, exist_ok=True)

    summary_rows = []
    started_all = time.perf_counter()
    with args.out.open("w", encoding="utf-8", newline="\n") as f:
        for row in rows:
            wav_path = (ROOT / row["file"]).resolve()
            started = time.perf_counter()
            error = None
            try:
                text = decode_one(recognizer, wav_path)
            except Exception as exc:
                text = ""
                error = f"{type(exc).__name__}: {exc}"
            elapsed_ms = int((time.perf_counter() - started) * 1000)
            cer = character_error_rate(row["expected"], text)
            hits = [word for word in row["hotwords"] if normalize_text(word) in normalize_text(text)]
            item = {
                "kind": args.kind,
                "uttid": row["uttid"],
                "file": row["file"],
                "expected": row["expected"],
                "actual": text,
                "cer": cer,
                "hotwords": row["hotwords"],
                "hotword_hits": hits,
                "elapsed_ms": elapsed_ms,
                "error": error,
            }
            summary_rows.append(item)
            f.write(json.dumps(item, ensure_ascii=False) + "\n")

    elapsed_ms = int((time.perf_counter() - started_all) * 1000)
    avg_cer = sum(item["cer"] for item in summary_rows) / len(summary_rows)
    total_hotwords = sum(len(item["hotwords"]) for item in summary_rows)
    hit_hotwords = sum(len(item["hotword_hits"]) for item in summary_rows)
    error_count = sum(1 for item in summary_rows if item.get("error"))
    print(f"sherpa-onnx {args.kind} rows: {len(summary_rows)}")
    print(f"Elapsed: {elapsed_ms} ms")
    print(f"Average CER: {avg_cer:.4f}")
    if total_hotwords:
        print(f"Hotword hits: {hit_hotwords}/{total_hotwords}")
    if error_count:
        print(f"Errors: {error_count}/{len(summary_rows)}")
    print(f"JSONL: {args.out}")
    return 0


def self_test() -> int:
    assert normalize_text(" DeepSeek，破防了！") == "deepseek破防了"
    assert character_error_rate("你好世界", "你好") == 0.5
    assert split_hotwords("DeepSeek, QNN；哈基米") == ["DeepSeek", "QNN", "哈基米"]
    print("sherpa_onnx_eval self-test passed")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Evaluate sherpa-onnx offline ASR models on PocketVoice wav manifests.")
    parser.add_argument("--kind", choices=["zipformer_ctc", "sense_voice", "paraformer", "firered_ctc", "firered_aed", "whisper", "qwen3_asr", "funasr_nano"], default="zipformer_ctc")
    parser.add_argument("--manifest", type=Path, default=ROOT / "test_audio" / "expected.tsv")
    parser.add_argument("--out", type=Path, default=ROOT / "build" / "test-results" / "sherpa-onnx-eval.jsonl")
    parser.add_argument("--model", default="")
    parser.add_argument("--tokens", default="")
    parser.add_argument("--encoder", default="")
    parser.add_argument("--decoder", default="")
    parser.add_argument("--conv-frontend", default="")
    parser.add_argument("--encoder-adaptor", default="")
    parser.add_argument("--llm", default="")
    parser.add_argument("--embedding", default="")
    parser.add_argument("--tokenizer", default="")
    parser.add_argument("--language", default="")
    parser.add_argument("--extra-hotwords", default="")
    parser.add_argument("--threads", type=int, default=2)
    parser.add_argument("--provider", default="cpu")
    parser.add_argument("--tail-paddings", type=int, default=-1)
    parser.add_argument("--max-total-len", type=int, default=512)
    parser.add_argument("--max-new-tokens", type=int, default=128)
    parser.add_argument("--limit", type=int, default=0)
    parser.add_argument("--use-itn", action="store_true")
    parser.add_argument("--debug", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()

    if args.self_test:
        return self_test()
    return evaluate(args)


if __name__ == "__main__":
    raise SystemExit(main())
