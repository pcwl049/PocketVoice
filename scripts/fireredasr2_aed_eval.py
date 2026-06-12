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
BLANK_ID = 0


def normalize_text(value: str) -> str:
    value = value.strip().lower()
    value = re.sub(r"\s+", "", value)
    value = re.sub(r"[，。！？、,.!?;；:：\"'“”‘’（）()\[\]【】<>《》]", "", value)
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


def collapse_ctc_ids(ids: list[int], blank_id: int = BLANK_ID) -> list[int]:
    collapsed = []
    prev = None
    for token_id in ids:
        if token_id != blank_id and token_id != prev:
            collapsed.append(token_id)
        prev = token_id
    return collapsed


def split_hotwords(raw: str) -> list[str]:
    if not raw:
        return []
    return [item.strip() for item in re.split(r"[,，;；|]", raw) if item.strip()]


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
            mode = row.get("mode", "wav")
            if mode != "wav":
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


def read_hotword_manifest(path: Path) -> list[dict]:
    rows = []
    with path.open("r", encoding="utf-8-sig", newline="") as f:
        reader = csv.DictReader(
            (line for line in f if line.strip() and not line.lstrip().startswith("#")),
            delimiter="\t",
        )
        for row in reader:
            rows.append(row)
    return rows


def load_firered(repo_dir: Path):
    if not repo_dir.exists():
        raise RuntimeError(
            f"FireRedASR2S repo not found: {repo_dir}\n"
            "Clone it with:\n"
            "  git clone https://github.com/FireRedTeam/FireRedASR2S.git third_party/FireRedASR2S\n"
            "Then install its requirements in your Python environment."
        )
    sys.path.insert(0, str(repo_dir))
    try:
        from fireredasr2s.fireredasr2 import FireRedAsr2, FireRedAsr2Config
    except Exception as exc:
        raise RuntimeError(
            "Could not import FireRedASR2S. Install dependencies from "
            f"{repo_dir / 'requirements.txt'} and make sure this script uses that Python."
        ) from exc
    return FireRedAsr2, FireRedAsr2Config


def transcribe_ctc_greedy(model, uttids: list[str], wav_paths: list[str]) -> list[dict]:
    import torch

    feats, lengths, durs, batch_wav_path, batch_uttid = model.feat_extractor(wav_paths, uttids)
    if feats is None:
        return [{"uttid": uttid, "text": ""} for uttid in uttids]

    if model.config.use_gpu:
        feats = feats.cuda()
        lengths = lengths.cuda()
        if model.config.use_half:
            if torch.cuda.is_bf16_supported():
                feats = feats.bfloat16()
            else:
                feats = feats.half()

    started = time.perf_counter()
    with torch.no_grad():
        enc_outputs, enc_lengths, _ = model.model.encoder(feats, lengths)
        logits = model.model.ctc(enc_outputs)
        best = logits.argmax(dim=2).cpu().tolist()
    elapsed = time.perf_counter() - started
    total_dur = sum(durs)
    rtf = elapsed / total_dur if total_dur > 0 else 0

    results = []
    for uttid, wav, dur, ids, enc_len in zip(batch_uttid, batch_wav_path, durs, best, enc_lengths.cpu().tolist()):
        token_ids = collapse_ctc_ids(ids[: int(enc_len)])
        text = model.tokenizer.detokenize(token_ids)
        text = re.sub(r"(<blank>)|(<sil>)", "", text)
        results.append({
            "uttid": uttid,
            "text": text.lower(),
            "dur_s": round(dur, 3),
            "rtf": f"{rtf:.4f}",
            "wav": wav,
            "decode": "ctc",
        })
    return results


def ensure_model(model_dir: Path):
    if model_dir.exists():
        return
    raise RuntimeError(
        f"FireRedASR2-AED model not found: {model_dir}\n"
        "Download it with one of:\n"
        "  modelscope download --model xukaituo/FireRedASR2-AED --local_dir models/fireredasr2-aed\n"
        "  huggingface-cli download FireRedTeam/FireRedASR2-AED --local-dir models/fireredasr2-aed"
    )


def evaluate(args) -> int:
    manifest = read_eval_manifest(args.manifest)
    if not manifest:
        raise RuntimeError(f"No wav rows found in {args.manifest}")

    repo_dir = args.repo
    model_dir = args.model
    ensure_model(model_dir)
    FireRedAsr2, FireRedAsr2Config = load_firered(repo_dir)

    config = FireRedAsr2Config(
        use_gpu=args.device == "cuda",
        use_half=args.half,
        beam_size=args.beam_size,
        nbest=1,
        decode_max_len=0,
        softmax_smoothing=1.25,
        aed_length_penalty=0.6,
        eos_penalty=1.0,
        return_timestamp=False,
    )
    model = FireRedAsr2.from_pretrained("aed", str(model_dir), config)

    rows = manifest[: args.limit] if args.limit else manifest
    wav_paths = [str((ROOT / row["file"]).resolve()) for row in rows]
    uttids = [row["uttid"] for row in rows]

    started = time.perf_counter()
    if args.decode == "aed":
        results = model.transcribe(uttids, wav_paths)
    else:
        results = transcribe_ctc_greedy(model, uttids, wav_paths)
    elapsed_ms = int((time.perf_counter() - started) * 1000)
    by_utt = {item.get("uttid"): item for item in results}

    args.out.parent.mkdir(parents=True, exist_ok=True)
    summary_rows = []
    with args.out.open("w", encoding="utf-8", newline="\n") as f:
        for row in rows:
            result = by_utt.get(row["uttid"], {})
            text = result.get("text", "")
            cer = character_error_rate(row["expected"], text)
            hotwords = row["hotwords"]
            hits = [word for word in hotwords if normalize_text(word) in normalize_text(text)]
            item = {
                "uttid": row["uttid"],
                "file": row["file"],
                "expected": row["expected"],
                "actual": text,
                "cer": cer,
                "hotwords": hotwords,
                "hotword_hits": hits,
                "confidence": result.get("confidence"),
                "rtf": result.get("rtf"),
                "raw": result,
                "decode": args.decode,
            }
            summary_rows.append(item)
            f.write(json.dumps(item, ensure_ascii=False) + "\n")

    avg_cer = sum(item["cer"] for item in summary_rows) / len(summary_rows)
    total_hotwords = sum(len(item["hotwords"]) for item in summary_rows)
    hit_hotwords = sum(len(item["hotword_hits"]) for item in summary_rows)
    print(f"FireRedASR2-{args.decode.upper()} rows: {len(summary_rows)}")
    print(f"Elapsed: {elapsed_ms} ms")
    print(f"Average CER: {avg_cer:.4f}")
    if total_hotwords:
        print(f"Hotword hits: {hit_hotwords}/{total_hotwords}")
    print(f"JSONL: {args.out}")
    return 0


def self_test() -> int:
    assert normalize_text(" DeepSeek，破防了！ ") == "deepseek破防了"
    assert character_error_rate("你好世界", "你好世界") == 0
    assert character_error_rate("你好世界", "你好") == 0.5
    assert split_hotwords("DeepSeek, QNN；哈基米") == ["DeepSeek", "QNN", "哈基米"]
    assert collapse_ctc_ids([0, 3, 3, 0, 3, 4, 4, 0]) == [3, 3, 4]
    print("fireredasr2_aed_eval self-test passed")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Evaluate FireRedASR2-AED on PocketVoice wav manifests.")
    parser.add_argument("--repo", type=Path, default=Path(os.environ.get("FIREREDASR2S_REPO", ROOT / "third_party" / "FireRedASR2S")))
    parser.add_argument("--model", type=Path, default=Path(os.environ.get("FIREREDASR2_AED_MODEL", ROOT / "models" / "fireredasr2-aed")))
    parser.add_argument("--manifest", type=Path, default=ROOT / "test_audio" / "expected.tsv")
    parser.add_argument("--hotword-manifest", type=Path, default=ROOT / "test_audio" / "hotwords.tsv")
    parser.add_argument("--out", type=Path, default=ROOT / "build" / "test-results" / "fireredasr2-aed-eval.jsonl")
    parser.add_argument("--device", choices=["cpu", "cuda"], default=os.environ.get("FIREREDASR2_DEVICE", "cpu"))
    parser.add_argument("--decode", choices=["aed", "ctc"], default="aed")
    parser.add_argument("--half", action="store_true")
    parser.add_argument("--beam-size", type=int, default=3)
    parser.add_argument("--limit", type=int, default=0)
    parser.add_argument("--list-hotwords", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()

    if args.self_test:
        return self_test()
    if args.list_hotwords:
        rows = read_hotword_manifest(args.hotword_manifest)
        for row in rows:
            print(json.dumps(row, ensure_ascii=False))
        print(f"Hotword rows: {len(rows)}")
        return 0
    return evaluate(args)


if __name__ == "__main__":
    raise SystemExit(main())
