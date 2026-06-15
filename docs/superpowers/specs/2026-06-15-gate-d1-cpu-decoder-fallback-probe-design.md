# Gate D1: CPU Decoder Fallback Probe Design

Date: 2026-06-15
Status: Approved
Parent: QWEN3_QNN_NEXT_TASKS_FOR_AI.md (Gate D)

## Context

Gate C2 proved the QNN HTP decoder has no observable KV-cache input influence in APK.
HTP graphFinalize overrides cache_key/cache_value quantization scale to 1.53e-9,
effectively disconnecting the KV cache path. All QNN override escape attempts (Gate A-C2)
have been exhausted.

The `Qwen3AsrCpu` backend already exists in `stt_engine.cpp` — it uses sherpa-onnx C API
+ ORT to run the full Qwen3 decoder on CPU. It can initialize on the target phone but
has never been measured for latency, memory, or correctness.

## Goal

Measure CPU decoder fallback feasibility on the target Android phone to get a
go/no-go signal for product viability.

## Measurement Method

APK-embedded probes in the existing `Qwen3AsrCpu` recognize path. No architecture
changes, no new backends. Only timing/memory logging added.

## Metrics

| Metric | How | Unit |
|--------|-----|------|
| Init time | Wall clock around `SherpaOnnxCreateOfflineRecognizer` | ms |
| Decode time | Wall clock around `SherpaOnnxOfflineRecognizerDecode` | ms |
| RSS before | `/proc/self/statm` before decode | KB |
| RSS after | `/proc/self/statm` after decode | KB |
| RSS delta | after - before | KB |
| Output text | sherpa-onnx result string | text |
| Audio duration | Sample count / 16000 | ms |

## Log Format

All logs use Android logcat tag `STT_GATE_D1`:

```
STT_GATE_D1: init_ms=<ms>
STT_GATE_D1: audio_duration_ms=<ms> audio_samples=<n>
STT_GATE_D1: decode_start
STT_GATE_D1: decode_done total_ms=<ms>
STT_GATE_D1: rss_before_kb=<kb> rss_after_kb=<kb> rss_delta_kb=<kb>
STT_GATE_D1: result="<text>"
```

## Code Changes

Only `src/mobile/app/src/main/cpp/stt_engine.cpp`:

1. Add `readRssKb()` helper — reads `/proc/self/statm` line 2 (resident pages * page_size / 1024)
2. In `Qwen3AsrCpu` init path: log `init_ms`
3. In `Qwen3AsrCpu` recognize path: log decode timing, RSS, and result

Estimated change: < 40 lines.

## Test Procedure

1. Build APK with `--cpu` flag: `scripts\build_mobile_apk.bat --cpu`
2. Install on connected phone
3. Send test WAV via PC client or `adb`
4. Filter logcat: `adb logcat -s STT_GATE_D1`
5. Record metrics
6. Compare output text with PC ORT reference for correctness

## Go/No-Go Criteria

| Verdict | Condition | Action |
|---------|-----------|--------|
| GO | Total decode < 10s, output matches reference | CPU fallback is viable, proceed to integrate as default decoder path |
| MARGINAL | 10-30s, output correct | Usable but poor UX, consider hybrid QNN-encoder + CPU-decoder or model optimization |
| NO-GO | > 30s or output wrong | CPU fallback not viable, evaluate Route D3 (different runtime/model) |

## Out of Scope

- Per-step decode timing (sherpa-onnx C API is a black box)
- Custom CPU decoder backend (only if GO and optimization needed)
- Hybrid QNN+CPU architecture (Route D2, deferred)
- Alternative runtimes (Route D3, deferred unless NO-GO)

## Required Result Format

```
Gate D1: CPU_FALLBACK_PASSED / CPU_FALLBACK_MARGINAL / CPU_FALLBACK_FAILED
Latency: <ms>
RSS delta: <kb>
Output: "<text>"
Next: <specific implementation route>
```
