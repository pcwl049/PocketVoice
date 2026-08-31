#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Multi-model Mandarin CER evaluation (aishell-1 test parquet or wav manifest).

Pipelines mirror sherpa-onnx exactly:
  fire_red   : knf fbank povey 80 bins, int16-range waveform, per-dim CMVN
               from ONNX metadata (cmvn_mean / cmvn_inv_stddev); CTC greedy.
  sense_voice: knf fbank hamming 80 bins, [-1,1] waveform, LFR 7/6 -> 560,
               CMVN (neg_mean / inv_stddev) after LFR; 4 inputs; CTC greedy.
  paraformer : knf fbank hamming 80 bins, int16-range waveform, LFR 7/6,
               CMVN (neg_mean / inv_stddev) after LFR; CIF outputs ->
               per-token argmax + </s> stop + BPE (@@) merge.

Usage:
  python multi_model_cer_eval.py --corpus aishell.parquet --n 120 \
      --fire-red-dir <dir> --sense-voice-dir <dir> \
      --paraformer-dir <dir> --paraformer-bilingual-dir <dir> \
      --out results.json
  (--corpus can be an aishell-style parquet with 'context' audio + 'answer'
   ref, or a TSV manifest: path<TAB>ref)
"""
import argparse
import io
import json
import re
import sys
import wave
from pathlib import Path

import numpy as np
import onnx
import onnxruntime as ort
import kaldi_native_fbank as knf

ROOT = Path(__file__).resolve().parents[1]


def load_tokens(path):
    id2sym = {}
    for line in open(path, encoding="utf-8"):
        parts = line.rstrip("\n").split(" ")
        if len(parts) >= 2:
            id2sym[int(parts[-1])] = " ".join(parts[:-1])
    return id2sym


def norm(s):
    return re.sub(r"[，。？！、：；,.?!:; \u3000<>|]", "", s).lower()


def cer(ref, hyp):
    r, h = norm(ref), norm(hyp)
    n, m = len(r), len(h)
    dp = list(range(m + 1))
    for i in range(1, n + 1):
        prev = dp[0]
        dp[0] = i
        for j in range(1, m + 1):
            cur = dp[j]
            dp[j] = min(dp[j] + 1, dp[j - 1] + 1, prev + (r[i - 1] != h[j - 1]))
            prev = cur
    return dp[m], n


def make_fbank(window):
    o = knf.FbankOptions()
    o.frame_opts.samp_freq = 16000.0
    o.frame_opts.frame_shift_ms = 10.0
    o.frame_opts.frame_length_ms = 25.0
    o.frame_opts.dither = 0.0
    o.frame_opts.preemph_coeff = 0.97
    o.frame_opts.remove_dc_offset = True
    o.frame_opts.window_type = window
    o.frame_opts.snip_edges = True
    o.mel_opts.num_bins = 80
    o.mel_opts.low_freq = 20.0
    o.mel_opts.high_freq = 0.0
    return knf.OnlineFbank(o)


def fbank_of(samples, window, int16scale):
    fb = make_fbank(window)
    s = np.asarray(samples, np.float32)
    if int16scale:
        s = s * 32768.0
    fb.accept_waveform(16000.0, s)
    fb.input_finished()
    T = fb.num_frames_ready
    out = np.empty((T, 80), np.float32)
    for i in range(T):
        out[i] = fb.get_frame(i)
    return out


def apply_lfr(frames, win, shift):
    T, D = frames.shape
    if T < win:
        return frames.reshape(1, -1)
    outT = (T - win) // shift + 1
    idx = np.arange(win)[None, :] + shift * np.arange(outT)[:, None]
    return frames[idx].reshape(outT, win * D)


def floats(s):
    return np.array([float(v) for v in s.split(",")], np.float32)


def ctc_decode(lp, id2sym, blank=0):
    ids = lp.argmax(-1).ravel()
    out, prev = [], -1
    for i in ids:
        i = int(i)
        if i != blank and i != prev:
            out.append(i)
        prev = i
    return "".join(id2sym.get(t, "?") for t in out)


def pf_decode(outs, id2sym, eos_id):
    lp = outs[0]
    seq = lp[0] if lp.ndim == 3 else lp
    n_tok = seq.shape[0]
    if len(outs) > 1 and outs[1].ndim >= 1:
        try:
            n_tok = int(outs[1].ravel()[0])
        except Exception:
            pass
    ids = []
    for k in range(min(n_tok, seq.shape[0])):
        b = int(np.argmax(seq[k]))
        if b == eos_id:
            break
        ids.append(b)
    # BPE merge: tokens ending with @@ concatenate without spaces
    parts = [id2sym.get(i, "?") for i in ids]
    merged = []
    for p_ in parts:
        if p_.endswith("@@"):
            merged.append(p_[:-2])
        else:
            merged.append(p_)
            merged.append(" ")
    return "".join(merged).strip()


def sv_decode(lp, id2sym):
    ids = lp[0] if lp.ndim == 2 else lp.argmax(-1).ravel()
    toks = [int(t) for t in ids]
    start = 4 if len(toks) > 4 else 0
    out, prev = [], -1
    for i in toks[start:]:
        if i != 0 and i != prev:
            out.append(id2sym.get(i, "?"))
        prev = i
    return "".join(out)


class ModelSet:
    def __init__(self, args):
        self.sessions = {}
        self.cfg = {}
        opts4 = ort.SessionOptions()
        opts4.intra_op_num_threads = 4
        providers = ["CPUExecutionProvider"]

        def meta(p):
            return {q.key: q.value for q in
                    onnx.load(p, load_external_data=False).metadata_props}

        if args.fire_red_dir:
            d = Path(args.fire_red_dir)
            self.sessions["fire_red"] = ort.InferenceSession(
                str(d / "model.int8.onnx"), opts4, providers=providers)
            m = meta(d / "model.int8.onnx")
            self.cfg["fire_red"] = {
                "tok": load_tokens(d / "tokens.txt"),
                "mean": floats(m["cmvn_mean"]), "inv": floats(m["cmvn_inv_stddev"]),
            }
        if args.sense_voice_dir:
            d = Path(args.sense_voice_dir)
            self.sessions["sense_voice"] = ort.InferenceSession(
                str(d / "model.int8.onnx"), opts4, providers=providers)
            m = meta(d / "model.int8.onnx")
            self.cfg["sense_voice"] = {
                "tok": load_tokens(d / "tokens.txt"),
                "win": int(m["lfr_window_size"]), "shift": int(m["lfr_window_shift"]),
                "neg": floats(m["neg_mean"]), "inv": floats(m["inv_stddev"]),
                "lang": int(m.get("lang_auto", 0)), "itn": int(m.get("with_itn", 9)),
            }
        if args.paraformer_dir:
            d = Path(args.paraformer_dir)
            self.sessions["paraformer"] = ort.InferenceSession(
                str(d / "model.int8.onnx"), opts4, providers=providers)
            m = meta(d / "model.int8.onnx")
            sym2id = {v: k for k, v in load_tokens(d / "tokens.txt").items()}
            self.cfg["paraformer"] = {
                "tok": load_tokens(d / "tokens.txt"),
                "win": int(m["lfr_window_size"]), "shift": int(m["lfr_window_shift"]),
                "neg": floats(m["neg_mean"]), "inv": floats(m["inv_stddev"]),
                "eos": sym2id.get("</s>", 1),
            }
        if args.paraformer_bilingual_dir:
            d = Path(args.paraformer_bilingual_dir)
            self.sessions["paraformer_bilingual"] = ort.InferenceSession(
                str(d / "model.int8.onnx"), opts4, providers=providers)
            m = meta(d / "model.int8.onnx")
            sym2id = {v: k for k, v in load_tokens(d / "tokens.txt").items()}
            self.cfg["paraformer_bilingual"] = {
                "tok": load_tokens(d / "tokens.txt"),
                "win": int(m["lfr_window_size"]), "shift": int(m["lfr_window_shift"]),
                "neg": floats(m["neg_mean"]), "inv": floats(m["inv_stddev"]),
                "eos": sym2id.get("</s>", 1),
            }

    def run(self, tag, x):
        c = self.cfg[tag]
        sess = self.sessions[tag]
        if tag == "fire_red":
            f = fbank_of(x, "povey", True)
            f = (f - c["mean"]) * c["inv"]
            lp = sess.run(None, {"x": f[None].astype(np.float32),
                                 "x_lens": np.array([f.shape[0]], np.int64)})[0]
            return ctc_decode(lp, c["tok"])
        if tag == "sense_voice":
            f = fbank_of(x, "hamming", False)
            f = apply_lfr(f, c["win"], c["shift"])
            f = (f + c["neg"]) * c["inv"]
            feed = {"x": f[None].astype(np.float32),
                    "x_length": np.array([f.shape[0]], np.int32),
                    "language": np.array([c["lang"]], np.int32),
                    "text_norm": np.array([c["itn"]], np.int32)}
            return sv_decode(sess.run(None, feed)[0], c["tok"])
        # paraformer family
        f = fbank_of(x, "hamming", True)
        f = apply_lfr(f, c["win"], c["shift"])
        f = (f + c["neg"]) * c["inv"]
        feed = {"speech": f[None].astype(np.float32),
                "speech_lengths": np.array([f.shape[0]], np.int32)}
        return pf_decode(sess.run(None, feed), c["tok"], c["eos"])


def load_corpus(path, n):
    path = Path(path)
    items = []
    if path.suffix == ".parquet":
        import pyarrow.parquet as pq
        t = pq.read_table(path)
        cols = t.column_names
        ka = "context" if "context" in cols else "audio"
        kt = "answer" if "answer" in cols else ("sentence" if "sentence" in cols else cols[-1])
        for row in range(min(n, t.num_rows)):
            rec = t.slice(row, 1).to_pylist()[0]
            audio = rec[ka]
            data = audio.get("bytes") if isinstance(audio, dict) else audio
            ref = rec[kt]
            items.append((data, ref))
    else:  # TSV manifest: path<TAB>ref
        for line in open(path, encoding="utf-8"):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            p_, _, ref = line.partition("\t")
            items.append((p_, ref))
            if len(items) >= n:
                break
    return items


def audio_to_float(data, ref):
    if isinstance(data, str):  # manifest path
        import soundfile as sf
        x, sr = sf.read(data, dtype="float32")
        if sr != 16000:
            x = np.interp(np.arange(len(x) * 16000 // sr) / 16000,
                          np.arange(len(x)) / sr, x).astype(np.float32)
        return x, ref
    w = wave.open(io.BytesIO(data))
    sw, ch = w.getsampwidth(), w.getnchannels()
    raw = w.readframes(w.getnframes())
    x = np.frombuffer(raw, np.int16).astype(np.float32) / 32768.0
    if ch > 1:
        x = x.reshape(-1, ch).mean(axis=1)
    return x, ref


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--corpus", type=Path, required=True,
                    help="aishell-style .parquet or TSV manifest (path<TAB>ref)")
    ap.add_argument("--n", type=int, default=120)
    ap.add_argument("--fire-red-dir", type=Path)
    ap.add_argument("--sense-voice-dir", type=Path)
    ap.add_argument("--paraformer-dir", type=Path)
    ap.add_argument("--paraformer-bilingual-dir", type=Path)
    ap.add_argument("--out", type=Path, default=ROOT / "build" / "test-results" / "multi-model-cer.json")
    args = ap.parse_args()

    ms = ModelSet(args)
    if not ms.sessions:
        print("no model dirs given", file=sys.stderr)
        return 1
    items = load_corpus(args.corpus, args.n)

    acc = {k: [0, 0] for k in ms.sessions}
    for idx, (data, ref) in enumerate(items):
        x, ref = audio_to_float(data, ref)
        if len(x) < 16000:
            continue
        for tag in ms.sessions:
            try:
                hyp = ms.run(tag, x)
            except Exception:
                hyp = ""
            e, n = cer(ref, hyp)
            acc[tag][0] += e
            acc[tag][1] += n
        if (idx + 1) % 20 == 0:
            print(f"progress {idx + 1}/{len(items)}", flush=True)

    result = {}
    for tag in ms.sessions:
        e, n = acc[tag]
        result[tag] = {"err": e, "n": n, "cer": e / max(1, n) * 100}
        print(f"{tag}: CER {e}/{n} = {e / max(1, n) * 100:.2f}%")
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(result, ensure_ascii=False, indent=1), encoding="utf-8")
    print("saved", args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
