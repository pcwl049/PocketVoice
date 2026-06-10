#include <jni.h>
#include <android/log.h>
#include <string>
#include <thread>
#include <atomic>
#include <chrono>
#include <mutex>
#include <cstdlib>
#include <sstream>
#include "stt_engine.h"
#include "network.h"
#include "runtime_state.h"
#include "audio_job_queue.h"

#define LOG_TAG "STT_Native"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static std::thread g_engineThread;
static std::atomic<bool> g_running{false};
static std::atomic<bool> g_serverReady{false};
static std::atomic<bool> g_starting{false};
static std::mutex g_threadMutex;
static std::mutex g_errorMutex;
static std::string g_modelDir;
static std::string g_nativeLibraryDir;
static std::string g_lastError;
static stt::RuntimeState g_runtimeState;
static std::atomic<bool> g_recognitionCacheEnabled{false};

static void recognizeOneAudio(stt::SttEngine& engine, stt::NetworkServer& server, const stt::AudioData& audio) {
    float duration = (float)audio.samples.size() / audio.sample_rate;
    LOGI("Audio cb: %.2f sec, %zu samples, segment=%u", duration, audio.samples.size(), audio.segment_id);

    if (duration < 0.3f) {
        LOGI("  Too short, skipping");
        return;
    }

    auto cached = g_runtimeState.findCached(audio.samples.data(), audio.samples.size());
    if (cached.hit) {
        g_runtimeState.recordCacheHit(audio.samples.data(), audio.samples.size(), audio.sample_rate, cached.text);
        LOGI("Recognize cache hit: \"%s\"", cached.text.c_str());
        if (!cached.text.empty()) {
            server.sendText(cached.text, "", "", audio.segment_id);
            LOGI("  Sent cached response");
        }
        return;
    }

    auto recognizeStart = std::chrono::steady_clock::now();
    auto result = engine.recognize(audio.samples.data(), audio.samples.size());
    auto recognizeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - recognizeStart
    ).count();

    if (result.success) {
        g_runtimeState.recordRecognition(
            audio.samples.data(),
            audio.samples.size(),
            audio.sample_rate,
            static_cast<int>(recognizeMs),
            result.text);
        LOGI("Recognize OK in %lld ms: \"%s\"", (long long)recognizeMs, result.text.c_str());
        if (!result.text.empty()) {
            server.sendText(result.text, result.emotion, result.event, audio.segment_id);
            LOGI("  Sent response");
        } else {
            LOGI("  Empty text, not sending");
        }
    } else {
        LOGI("Recognize failed in %lld ms", (long long)recognizeMs);
    }
}

static void setLastError(const char* message) {
    std::lock_guard<std::mutex> lock(g_errorMutex);
    g_lastError = message;
}

static std::string getStatusString() {
    if (g_serverReady) return "running";
    if (g_starting) return "starting";
    std::lock_guard<std::mutex> lock(g_errorMutex);
    if (!g_lastError.empty()) return "error: " + g_lastError;
    return "stopped";
}

static std::string escapeJson(const std::string& value) {
    std::ostringstream os;
    for (unsigned char c : value) {
        switch (c) {
            case '"': os << "\\\""; break;
            case '\\': os << "\\\\"; break;
            case '\b': os << "\\b"; break;
            case '\f': os << "\\f"; break;
            case '\n': os << "\\n"; break;
            case '\r': os << "\\r"; break;
            case '\t': os << "\\t"; break;
            default:
                if (c < 0x20) {
                    const char* digits = "0123456789abcdef";
                    os << "\\u00" << digits[(c >> 4) & 0x0f] << digits[c & 0x0f];
                } else {
                    os << static_cast<char>(c);
                }
        }
    }
    return os.str();
}

static std::string buildRuntimeSnapshotJson() {
    auto snapshot = g_runtimeState.snapshot();
    std::ostringstream os;
    os << "{";
    os << "\"lastText\":\"" << escapeJson(snapshot.lastText) << "\",";
    os << "\"lastAudioMs\":" << snapshot.lastAudioMs << ",";
    os << "\"lastRecognizeMs\":" << snapshot.lastRecognizeMs << ",";
    os << "\"lastUpdatedMs\":" << snapshot.lastUpdatedMs << ",";
    os << "\"totalRequests\":" << snapshot.totalRequests << ",";
    os << "\"cacheHits\":" << snapshot.cacheHits << ",";
    os << "\"cacheEnabled\":" << (g_recognitionCacheEnabled.load() ? "true" : "false") << ",";
    os << "\"history\":[";
    for (size_t i = 0; i < snapshot.history.size(); ++i) {
        const auto& item = snapshot.history[i];
        if (i > 0) os << ",";
        os << "{";
        os << "\"text\":\"" << escapeJson(item.text) << "\",";
        os << "\"audioMs\":" << item.audioMs << ",";
        os << "\"recognizeMs\":" << item.recognizeMs << ",";
        os << "\"updatedMs\":" << item.updatedMs << ",";
        os << "\"cacheHit\":" << (item.cacheHit ? "true" : "false");
        os << "}";
    }
    os << "]}";
    return os.str();
}

static void stopEngineThreadLocked() {
    g_running = false;
    if (g_engineThread.joinable()) {
        g_engineThread.join();
    }
    g_starting = false;
    g_serverReady = false;
}

static void runEngine() {
    LOGI("Engine thread started");
    g_serverReady = false;
    g_starting = true;

    stt::SttEngine engine;
    g_runtimeState.setRecognitionCacheEnabled(g_recognitionCacheEnabled.load());
    if (!engine.init(g_modelDir, g_nativeLibraryDir)) {
        LOGE("Failed to init engine");
        setLastError("engine init failed");
        g_running = false;
        g_starting = false;
        return;
    }
    LOGI("Recognizer backend: %s", engine.backendName().c_str());

    stt::NetworkServer server;
    stt::AudioJobQueue audioQueue(1);
    std::thread recognizerWorker([&]() {
        stt::AudioData audio;
        while (audioQueue.waitPop(audio)) {
            recognizeOneAudio(engine, server, audio);
        }
        LOGI("Recognizer worker stopped");
    });

    server.setAudioCallback([&audioQueue](const stt::AudioData& audio) {
        if (!audioQueue.tryPush(audio)) {
            float duration = (float)audio.samples.size() / audio.sample_rate;
            LOGI("Audio queue full, dropping %.2f sec, %zu samples, segment=%u", duration, audio.samples.size(), audio.segment_id);
        }
    });

    if (!server.start(27000)) {
        LOGE("Failed to start server on port 27000");
        setLastError("server start failed");
        g_running = false;
        g_starting = false;
        return;
    }

    LOGI("Server listening on port 27000");
    g_starting = false;
    g_serverReady = true;
    setLastError("");

    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    server.stop();
    audioQueue.stop();
    if (recognizerWorker.joinable()) {
        recognizerWorker.join();
    }
    g_serverReady = false;
    LOGI("Engine thread stopped");
}

extern "C" {

JNIEXPORT jboolean JNICALL
Java_com_stt_mobile_MainActivity_nativeInit(JNIEnv* env, jobject /*thiz*/, jstring modelDir, jstring nativeLibraryDir) {
    const char* dir = env->GetStringUTFChars(modelDir, nullptr);
    g_modelDir = dir;
    env->ReleaseStringUTFChars(modelDir, dir);

    const char* libDir = env->GetStringUTFChars(nativeLibraryDir, nullptr);
    g_nativeLibraryDir = libDir;
    env->ReleaseStringUTFChars(nativeLibraryDir, libDir);

    if (!g_nativeLibraryDir.empty()) {
        std::string dspPath = g_nativeLibraryDir + ";" + g_modelDir
            + ";/vendor/dsp/cdsp;/vendor/lib/rfsa/adsp;/system/lib/rfsa/adsp;/dsp";
        setenv("ADSP_LIBRARY_PATH", dspPath.c_str(), 1);
        setenv("LD_LIBRARY_PATH", g_nativeLibraryDir.c_str(), 1);
        LOGI("ADSP_LIBRARY_PATH: %s", dspPath.c_str());
    }
    setLastError("");
    LOGI("nativeInit: %s", g_modelDir.c_str());
    return JNI_TRUE;
}

JNIEXPORT void JNICALL
Java_com_stt_mobile_MainActivity_nativeStart(JNIEnv* /*env*/, jobject /*thiz*/) {
    std::lock_guard<std::mutex> lock(g_threadMutex);
    if (g_running || g_starting) return;
    if (g_engineThread.joinable()) {
        g_engineThread.join();
    }
    setLastError("");
    g_serverReady = false;
    g_starting = true;
    g_running = true;
    g_engineThread = std::thread(runEngine);
}

JNIEXPORT void JNICALL
Java_com_stt_mobile_MainActivity_nativeStop(JNIEnv* /*env*/, jobject /*thiz*/) {
    LOGI("nativeStop");
    std::lock_guard<std::mutex> lock(g_threadMutex);
    stopEngineThreadLocked();
}

JNIEXPORT jstring JNICALL
Java_com_stt_mobile_MainActivity_nativeGetStatus(JNIEnv* env, jobject /*thiz*/) {
    std::string status = getStatusString();
    return env->NewStringUTF(status.c_str());
}

JNIEXPORT jstring JNICALL
Java_com_stt_mobile_MainActivity_nativeGetRuntimeSnapshot(JNIEnv* env, jobject /*thiz*/) {
    std::string json = buildRuntimeSnapshotJson();
    return env->NewStringUTF(json.c_str());
}

JNIEXPORT void JNICALL
Java_com_stt_mobile_MainActivity_nativeSetRecognitionCacheEnabled(JNIEnv* /*env*/, jobject /*thiz*/, jboolean enabled) {
    const bool value = enabled == JNI_TRUE;
    g_recognitionCacheEnabled = value;
    g_runtimeState.setRecognitionCacheEnabled(value);
    LOGI("Recognition cache %s", value ? "enabled" : "disabled");
}

}
