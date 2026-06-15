# Gate D1: CPU Decoder Fallback Probe Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add timing and memory probes to the Qwen3AsrCpu path in stt_engine.cpp to measure CPU decoder feasibility on the target Android phone.

**Architecture:** Pure probe injection — no architecture changes. Add `readRssKb()` helper and wall-clock timing around existing `SherpaOnnxCreateOfflineRecognizer` (init) and `SherpaOnnxDecodeOfflineStream` (decode) calls in the `Qwen3AsrCpu` backend path.

**Tech Stack:** C++20, Android NDK, `<chrono>`, `/proc/self/statm`, Android logcat (`__android_log_print`)

---

## File Structure

| Action | File | Responsibility |
|--------|------|----------------|
| Modify | `src/mobile/app/src/main/cpp/stt_engine.cpp` | Add `readRssKb()` helper, init timing, decode timing + RSS probes |

No new files created. Only `stt_engine.cpp` is touched.

---

### Task 1: Add readRssKb() helper function

**Files:**
- Modify: `src/mobile/app/src/main/cpp/stt_engine.cpp` (insert in anonymous namespace, after `readCpuFallbackThreads` at ~line 466)

- [ ] **Step 1: Add the helper function inside the anonymous namespace**

Insert after the closing brace of `readCpuFallbackThreads()` (line 466):

```cpp
static long readRssKb() {
    std::ifstream is("/proc/self/statm");
    if (!is) return 0;
    long size = 0, resident = 0;
    is >> size >> resident;
    // resident is in pages; page size on Android ARM64 is typically 4096
    return resident * 4;  // 4096 / 1024 = 4 KB per page
}
```

- [ ] **Step 2: Commit**

```bash
git add src/mobile/app/src/main/cpp/stt_engine.cpp
git commit -m "feat(gate-d1): add readRssKb() helper for RSS memory measurement"
```

---

### Task 2: Add init timing probe to Qwen3AsrCpu path

**Files:**
- Modify: `src/mobile/app/src/main/cpp/stt_engine.cpp:730-769` (the `#if STT_USE_QNN` / `useQwen3Asr` block)

- [ ] **Step 1: Add chrono include and log macro (if not already present)**

Already present: `#include <chrono>` (line 12) and `LOGI` macro (line 21). No action needed.

- [ ] **Step 2: Add init timing around SherpaOnnxCreateOfflineRecognizer**

In the `useQwen3Asr` block (starting at line 731), wrap the `SherpaOnnxCreateOfflineRecognizer` call with timing:

Replace the block from `LOGI("Creating offline Qwen3-ASR recognizer...");` through `return true;`:

```cpp
        LOGI("Selected backend: %s", m_backendName.c_str());
        LOGI("CPU fallback threads: %d", cpuFallbackThreads);
        LOGI("Qwen3 hotwords: %s", hotwords.empty() ? "empty" : "configured");
        LOGI("Creating offline Qwen3-ASR recognizer...");

        const auto initStart = std::chrono::steady_clock::now();
        m_impl->offlineRecognizer = SherpaOnnxCreateOfflineRecognizer(&config);
        const auto initEnd = std::chrono::steady_clock::now();
        const long initMs = std::chrono::duration_cast<std::chrono::milliseconds>(initEnd - initStart).count();

        if (!m_impl->offlineRecognizer) {
            LOGE("Failed to create offline Qwen3-ASR recognizer");
            return false;
        }

        __android_log_print(ANDROID_LOG_INFO, "STT_GATE_D1", "init_ms=%ld", initMs);
        m_initialized = true;
        LOGI("Initialized OK, backend=%s", m_backendName.c_str());
        return true;
```

Note: Use `__android_log_print` directly with tag `"STT_GATE_D1"` instead of `LOGI` to keep probe logs on a separate, easily filterable tag.

- [ ] **Step 3: Commit**

```bash
git add src/mobile/app/src/main/cpp/stt_engine.cpp
git commit -m "feat(gate-d1): add init timing probe to Qwen3AsrCpu path"
```

---

### Task 3: Add decode timing and RSS probes to Qwen3AsrCpu recognize path

**Files:**
- Modify: `src/mobile/app/src/main/cpp/stt_engine.cpp:869-890` (the `Qwen3AsrCpu` recognize block)

- [ ] **Step 1: Add decode timing + RSS measurement around the decode call**

Replace the `Qwen3AsrCpu` recognize block (lines 869-890) with:

```cpp
    if (m_backendType == BackendType::Qwen3AsrCpu) {
        if (!m_impl->offlineRecognizer) return result;

        const long audioDurationMs = static_cast<long>(numSamples * 1000 / 16000);
        __android_log_print(ANDROID_LOG_INFO, "STT_GATE_D1",
            "audio_duration_ms=%ld audio_samples=%zu", audioDurationMs, numSamples);

        const SherpaOnnxOfflineStream* stream = SherpaOnnxCreateOfflineStream(m_impl->offlineRecognizer);
        if (!stream) return result;

        SherpaOnnxAcceptWaveformOffline(stream, 16000, samples, static_cast<int32_t>(numSamples));

        const long rssBefore = readRssKb();
        __android_log_print(ANDROID_LOG_INFO, "STT_GATE_D1", "decode_start");

        const auto decodeStart = std::chrono::steady_clock::now();
        SherpaOnnxDecodeOfflineStream(m_impl->offlineRecognizer, stream);
        const auto decodeEnd = std::chrono::steady_clock::now();
        const long decodeMs = std::chrono::duration_cast<std::chrono::milliseconds>(decodeEnd - decodeStart).count();

        const long rssAfter = readRssKb();
        __android_log_print(ANDROID_LOG_INFO, "STT_GATE_D1",
            "decode_done total_ms=%ld", decodeMs);
        __android_log_print(ANDROID_LOG_INFO, "STT_GATE_D1",
            "rss_before_kb=%ld rss_after_kb=%ld rss_delta_kb=%ld",
            rssBefore, rssAfter, rssAfter - rssBefore);

        const SherpaOnnxOfflineRecognizerResult* res = SherpaOnnxGetOfflineStreamResult(stream);

        if (res) {
            result.success = true;
            if (res->text) result.text = res->text;
            __android_log_print(ANDROID_LOG_INFO, "STT_GATE_D1",
                "result=\"%s\"", result.text.c_str());
            LOGI("Result: \"%s\"", result.text.c_str());
            LOGI("JSON: %s", res->json ? res->json : "null");
            SherpaOnnxDestroyOfflineRecognizerResult(res);
        } else {
            __android_log_print(ANDROID_LOG_INFO, "STT_GATE_D1", "result=null");
            LOGI("Result is null");
        }
        SherpaOnnxDestroyOfflineStream(stream);
        return result;
    }
```

- [ ] **Step 2: Commit**

```bash
git add src/mobile/app/src/main/cpp/stt_engine.cpp
git commit -m "feat(gate-d1): add decode timing and RSS probes to Qwen3AsrCpu recognize path"
```

---

### Task 4: Build and deploy to phone

**Files:** None (build & deploy only)

- [ ] **Step 1: Build APK with CPU mode**

Run: `scripts\build_mobile_apk.bat --cpu`

Expected: Build succeeds, APK at `build\mobile-apk\app-signed.apk`

- [ ] **Step 2: Install APK on connected phone**

Run: `adb install -r build\mobile-apk\app-signed.apk`

Expected: "Success"

- [ ] **Step 3: Start logcat filter before sending audio**

Run: `adb logcat -s STT_GATE_D1`

Expected: Ready to capture probe output

- [ ] **Step 4: Send test WAV from PC client**

Use `build\pc\stt_pc.exe --wav <test_wav_path>` or another method to trigger recognition.

- [ ] **Step 5: Record probe output from logcat**

Expected output format:
```
STT_GATE_D1: init_ms=<N>
STT_GATE_D1: audio_duration_ms=<N> audio_samples=<N>
STT_GATE_D1: decode_start
STT_GATE_D1: decode_done total_ms=<N>
STT_GATE_D1: rss_before_kb=<N> rss_after_kb=<N> rss_delta_kb=<N>
STT_GATE_D1: result="<text>"
```

- [ ] **Step 6: Evaluate go/no-go**

| Verdict | Condition |
|---------|-----------|
| GO | decode < 10000ms, output is coherent text |
| MARGINAL | 10000-30000ms, output correct |
| NO-GO | > 30000ms or output garbage |

- [ ] **Step 7: Commit results to experiment log**

Update `docs/architecture/QWEN3_QNN_DECODER_EXPERIMENT_LOG.md` with Step 28 (Gate D1 results).

---

## Self-Review

1. **Spec coverage**: ✅ Init timing (Task 2), decode timing + RSS (Task 3), audio duration (Task 3), result text (Task 3), log format matches spec (STT_GATE_D1 tag), build + test (Task 4)
2. **Placeholder scan**: ✅ No TBD/TODO. All code blocks contain complete implementation.
3. **Type consistency**: ✅ `readRssKb()` returns `long`, all timing uses `long` + `std::chrono::milliseconds`, `__android_log_print` format specifiers match (`%ld` for `long`, `%zu` for `size_t`).
