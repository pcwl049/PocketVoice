#include "stt_engine.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <thread>
#include <chrono>
#ifndef STT_ENGINE_METADATA_ONLY
#include <android/log.h>
#endif

#ifndef STT_ENGINE_METADATA_ONLY
#define LOG_TAG "STT_Engine"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#else
#define LOGI(...)
#define LOGE(...)
#endif

#ifndef STT_ENGINE_METADATA_ONLY
extern "C" {
#include "sherpa-onnx/c-api/c-api.h"
}
#endif

namespace stt {

static std::string stripSenseVoiceToken(std::string value) {
    if (value.rfind("<|", 0) == 0 && value.size() > 4 && value.substr(value.size() - 2) == "|>") {
        return value.substr(2, value.size() - 4);
    }
    return value;
}

static std::string jsonStringValue(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return "";
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return "";
    pos = json.find('"', pos + 1);
    if (pos == std::string::npos) return "";
    size_t end = json.find('"', pos + 1);
    if (end == std::string::npos) return "";
    return json.substr(pos + 1, end - pos - 1);
}

SenseVoiceMetadata parseSenseVoiceMetadata(const std::string& json) {
    SenseVoiceMetadata metadata;
    std::string emotion = stripSenseVoiceToken(jsonStringValue(json, "emotion"));
    std::string event = stripSenseVoiceToken(jsonStringValue(json, "event"));
    if (!emotion.empty()) metadata.emotion = emotion;
    if (!event.empty()) metadata.event = event;
    return metadata;
}

#ifndef STT_ENGINE_METADATA_ONLY

struct SttEngine::Impl {
    const SherpaOnnxOnlineRecognizer* recognizer = nullptr;
    const SherpaOnnxOfflineRecognizer* offlineRecognizer = nullptr;
};

static bool fileExists(const std::string& path) {
    return SherpaOnnxFileExists(path.c_str());
}

static int readPositiveIntFile(const std::string& path) {
    std::ifstream is(path, std::ios::binary);
    if (!is) return 0;
    int value = 0;
    is >> value;
    return value > 0 ? value : 0;
}

static int readQnnVtcmMb(const std::string& modelDir) {
    int fileValue = readPositiveIntFile(modelDir + "/qnn_vtcm_mb.txt");
    if (fileValue > 0) return fileValue;
    const char* raw = std::getenv("STT_QNN_VTCM_MB");
    if (!raw || !*raw) return 16;
    int value = std::atoi(raw);
    if (value <= 0) return 16;
    return value;
}

static int clampCpuFallbackThreads(int value) {
    if (value < 2) return 2;
    if (value > 4) return 4;
    return value;
}

static int readCpuFallbackThreads(const std::string& modelDir) {
    int fileValue = readPositiveIntFile(modelDir + "/cpu_threads.txt");
    if (fileValue > 0) return clampCpuFallbackThreads(fileValue);
    const char* raw = std::getenv("STT_CPU_FALLBACK_THREADS");
    if (!raw || !*raw) return 2;
    int value = std::atoi(raw);
    if (value <= 0) return 2;
    return clampCpuFallbackThreads(value);
}

static bool writeQnnHtpConfig(const std::string& path, const std::string& modelDir) {
    std::ofstream os(path, std::ios::binary | std::ios::trunc);
    if (!os) return false;

    int vtcmMb = readQnnVtcmMb(modelDir);

    os <<
        "{\n"
        "  \"graphs\": [\n"
        "    {\n"
        "      \"vtcm_mb\": " << vtcmMb << ",\n"
        "      \"O\": 3,\n"
        "      \"graph_names\": [\"model\"]\n"
        "    }\n"
        "  ],\n"
        "  \"devices\": [\n"
        "    {\n"
        "      \"device_id\": 0,\n"
        "      \"soc_id\": 85,\n"
        "      \"dsp_arch\": \"v79\",\n"
        "      \"cores\": [\n"
        "        {\n"
        "          \"core_id\": 0,\n"
        "          \"perf_profile\": \"burst\",\n"
        "          \"rpc_control_latency\": 200\n"
        "        }\n"
        "      ]\n"
        "    }\n"
        "  ]\n"
        "}\n";
    return os.good();
}

SttEngine::SttEngine() : m_impl(new Impl()) {}

SttEngine::~SttEngine() {
    if (m_impl->recognizer) {
        SherpaOnnxDestroyOnlineRecognizer(m_impl->recognizer);
    }
    if (m_impl->offlineRecognizer) {
        SherpaOnnxDestroyOfflineRecognizer(m_impl->offlineRecognizer);
    }
    delete m_impl;
}

bool SttEngine::init(const std::string& modelDir, const std::string& qnnLibDir) {
    std::string senseVoiceQnnPath = modelDir + "/model.bin";
    std::string senseVoiceQnnLibPath = modelDir + "/libmodel.so";
    std::string packagedSenseVoiceQnnLibPath = qnnLibDir.empty() ? "libmodel.so" : (qnnLibDir + "/libmodel.so");
    std::string zipformerPath = modelDir + "/model.int8.onnx";
    std::string bbpePath = modelDir + "/bbpe.model";
    std::string tokensPath = modelDir + "/tokens.txt";
    std::string encoderPath = modelDir + "/encoder.int8.onnx";
    std::string decoderPath = modelDir + "/decoder.int8.onnx";
    
    LOGI("Check files...");
    LOGI("  model dir: %s", modelDir.c_str());
    LOGI("  sensevoice qnn model.bin: %s", fileExists(senseVoiceQnnPath) ? "exists" : "MISSING");
    LOGI("  sensevoice qnn libmodel.so: %s", fileExists(senseVoiceQnnLibPath) ? "exists" : "MISSING");
    LOGI("  zipformer model.int8.onnx: %s", fileExists(zipformerPath) ? "exists" : "MISSING");
    LOGI("  zipformer bbpe.model: %s", fileExists(bbpePath) ? "exists" : "MISSING");
    LOGI("  tokens.txt: %s", fileExists(tokensPath) ? "exists" : "MISSING");
    LOGI("  paraformer encoder.int8.onnx: %s", fileExists(encoderPath) ? "exists" : "MISSING");
    LOGI("  paraformer decoder.int8.onnx: %s", fileExists(decoderPath) ? "exists" : "MISSING");

    bool hasSenseVoiceContext = fileExists(senseVoiceQnnPath);
    bool hasSenseVoiceLib = fileExists(senseVoiceQnnLibPath);
    bool useSenseVoiceQnn = (hasSenseVoiceContext || hasSenseVoiceLib) && fileExists(tokensPath);
    bool useZipformer = fileExists(zipformerPath) && fileExists(tokensPath) && fileExists(bbpePath);
    bool useParaformer = fileExists(encoderPath) && fileExists(decoderPath) && fileExists(tokensPath);

#if STT_USE_QNN
    if (useSenseVoiceQnn) {
        m_backendType = BackendType::SenseVoiceQnn;
        m_backendName = "sensevoice_qnn";

        std::string htpExtensionsLib = qnnLibDir.empty()
            ? "libQnnHtpNetRunExtensions.so"
            : (qnnLibDir + "/libQnnHtpNetRunExtensions.so");
        std::string qnnBackendLib = qnnLibDir.empty()
            ? "libQnnHtp.so"
            : (qnnLibDir + "/libQnnHtp.so");
        if (!fileExists(qnnBackendLib)) {
            qnnBackendLib = modelDir + "/libQnnHtp.so";
        }
        if (!fileExists(qnnBackendLib)) {
            qnnBackendLib = "libQnnHtp.so";
        }
        std::string qnnSystemLib = qnnLibDir.empty()
            ? "libQnnSystem.so"
            : (qnnLibDir + "/libQnnSystem.so");
        if (!fileExists(qnnSystemLib)) {
            qnnSystemLib = modelDir + "/libQnnSystem.so";
        }
        if (!fileExists(qnnSystemLib)) {
            qnnSystemLib = "libQnnSystem.so";
        }
        std::string htpConfigPath = modelDir + "/htp_config.json";
        if (writeQnnHtpConfig(htpConfigPath, modelDir)) {
            setenv("SHERPA_ONNX_QNN_HTP_EXTENSIONS_LIB", htpExtensionsLib.c_str(), 1);
            setenv("SHERPA_ONNX_QNN_HTP_EXTENSIONS_CONFIG", htpConfigPath.c_str(), 1);
            unsetenv("SHERPA_ONNX_QNN_HTP_SIGNED_PD");
            LOGI("QNN HTP extensions lib: %s", htpExtensionsLib.c_str());
            LOGI("QNN HTP config: %s", htpConfigPath.c_str());
            LOGI("QNN HTP vtcm_mb: %d", readQnnVtcmMb(modelDir));
        } else {
            LOGE("Failed to write QNN HTP config: %s", htpConfigPath.c_str());
        }

        SherpaOnnxOfflineRecognizerConfig config;
        memset(&config, 0, sizeof(config));
        config.feat_config.sample_rate = 16000;
        config.feat_config.feature_dim = 80;
        if (fileExists(packagedSenseVoiceQnnLibPath)) {
            config.model_config.sense_voice.model = packagedSenseVoiceQnnLibPath.c_str();
        } else if (hasSenseVoiceLib) {
            config.model_config.sense_voice.model = senseVoiceQnnLibPath.c_str();
        }
        config.model_config.sense_voice.language = "auto";
        config.model_config.sense_voice.use_itn = 1;
        config.model_config.sense_voice.qnn_backend_lib = qnnBackendLib.c_str();
        config.model_config.sense_voice.qnn_context_binary = senseVoiceQnnPath.c_str();
        config.model_config.sense_voice.qnn_system_lib = qnnSystemLib.c_str();
        config.model_config.tokens = tokensPath.c_str();
        config.model_config.num_threads = 1;
        config.model_config.provider = "qnn";
        config.model_config.debug = 0;
        config.decoding_method = "greedy_search";

        LOGI("Selected backend: %s", m_backendName.c_str());
        LOGI("QNN model lib: %s", fileExists(packagedSenseVoiceQnnLibPath) ? packagedSenseVoiceQnnLibPath.c_str() : (hasSenseVoiceLib ? senseVoiceQnnLibPath.c_str() : "MISSING"));
        LOGI("QNN backend lib: %s", qnnBackendLib.c_str());
        LOGI("QNN system lib: %s", qnnSystemLib.c_str());
        LOGI("QNN context binary: %s", hasSenseVoiceContext ? senseVoiceQnnPath.c_str() : "not present; init from libmodel.so");
        LOGI("Creating offline QNN recognizer...");
        m_impl->offlineRecognizer = SherpaOnnxCreateOfflineRecognizer(&config);
        if (!m_impl->offlineRecognizer) {
            LOGE("Failed to create offline QNN recognizer");
            return false;
        }
        m_initialized = true;
        LOGI("Initialized OK, backend=%s", m_backendName.c_str());
        return true;
    }
#else
    if (useSenseVoiceQnn) {
        LOGI("SenseVoice QNN model present but APK was built without STT_USE_QNN");
    }
#endif

    if (!useZipformer && !useParaformer) {
        LOGE("No supported model set found in %s", modelDir.c_str());
        LOGE("Expected SenseVoice QNN: model.bin/libmodel.so + tokens.txt");
        LOGE("Expected Zipformer CTC: model.int8.onnx + bbpe.model + tokens.txt");
        LOGE("Expected Paraformer fallback: encoder.int8.onnx + decoder.int8.onnx + tokens.txt");
        return false;
    }
    
    SherpaOnnxOnlineRecognizerConfig config;
    memset(&config, 0, sizeof(config));
    
    config.feat_config.sample_rate = 16000;
    config.feat_config.feature_dim = 80;

    if (useZipformer) {
        m_backendType = BackendType::ZipformerCtc;
        m_backendName = "zipformer_ctc";
        config.model_config.zipformer2_ctc.model = zipformerPath.c_str();
        config.model_config.model_type = "zipformer2_ctc";
        LOGI("Selected backend: %s", m_backendName.c_str());
    } else {
        m_backendType = BackendType::Paraformer;
        m_backendName = "paraformer";
        config.model_config.paraformer.encoder = encoderPath.c_str();
        config.model_config.paraformer.decoder = decoderPath.c_str();
        config.model_config.model_type = "paraformer";
        LOGI("Selected backend: %s", m_backendName.c_str());
    }

    const int cpuFallbackThreads = readCpuFallbackThreads(modelDir);
    config.model_config.tokens = tokensPath.c_str();
    config.model_config.num_threads = cpuFallbackThreads;
    config.model_config.provider = "cpu";
    config.model_config.debug = 0;
    
    config.decoding_method = "greedy_search";
    config.enable_endpoint = 1;
    config.rule1_min_trailing_silence = 0.5f;
    config.rule2_min_trailing_silence = 1.0f;
    config.rule3_min_utterance_length = 3.0f;
    
    LOGI("CPU fallback threads: %d", cpuFallbackThreads);
    LOGI("Creating online recognizer...");
    m_impl->recognizer = SherpaOnnxCreateOnlineRecognizer(&config);
    
    if (!m_impl->recognizer) {
        LOGE("Failed to create online recognizer");
        return false;
    }
    
    m_initialized = true;
    LOGI("Initialized OK, backend=%s", m_backendName.c_str());
    return true;
}

RecognizeResult SttEngine::recognize(const float* samples, size_t numSamples) {
    RecognizeResult result;
    if (!m_initialized) return result;

#if STT_USE_QNN
    if (m_backendType == BackendType::SenseVoiceQnn) {
        if (!m_impl->offlineRecognizer) return result;

        const SherpaOnnxOfflineStream* stream = SherpaOnnxCreateOfflineStream(m_impl->offlineRecognizer);
        if (!stream) return result;

        SherpaOnnxAcceptWaveformOffline(stream, 16000, samples, static_cast<int32_t>(numSamples));
        SherpaOnnxDecodeOfflineStream(m_impl->offlineRecognizer, stream);
        const SherpaOnnxOfflineRecognizerResult* res = SherpaOnnxGetOfflineStreamResult(stream);

        if (res) {
            result.success = true;
            if (res->text) result.text = res->text;
            if (res->json) {
                auto metadata = parseSenseVoiceMetadata(res->json);
                result.emotion = metadata.emotion;
                result.event = metadata.event;
            }
            LOGI("Result: \"%s\"", result.text.c_str());
            LOGI("JSON: %s", res->json ? res->json : "null");
            SherpaOnnxDestroyOfflineRecognizerResult(res);
        } else {
            LOGI("Result is null");
        }
        SherpaOnnxDestroyOfflineStream(stream);
        return result;
    }
#endif

    if (!m_impl->recognizer) return result;
    
    const SherpaOnnxOnlineStream* stream = SherpaOnnxCreateOnlineStream(m_impl->recognizer);
    if (!stream) return result;
    
    // Feed audio in chunks with interleaved decode
    size_t offset = 0;
    const size_t chunk = 1600; // 0.1 second chunks
    while (offset < numSamples) {
        size_t n = (offset + chunk > numSamples) ? (numSamples - offset) : chunk;
        SherpaOnnxOnlineStreamAcceptWaveform(stream, 16000, samples + offset, (int32_t)n);
        offset += n;
        int d = 5;
        while (d-- > 0 && SherpaOnnxIsOnlineStreamReady(m_impl->recognizer, stream)) {
            SherpaOnnxDecodeOnlineStream(m_impl->recognizer, stream);
        }
    }
    
    // Signal end of input and finalize
    SherpaOnnxOnlineStreamInputFinished(stream);
    SherpaOnnxOnlineStreamSetOption(stream, "is_final", "1");
    
    int d = 30;
    while (d-- > 0 && SherpaOnnxIsOnlineStreamReady(m_impl->recognizer, stream)) {
        SherpaOnnxDecodeOnlineStream(m_impl->recognizer, stream);
    }
    
    // Try getting result multiple times
    const SherpaOnnxOnlineRecognizerResult* res = nullptr;
    for (int i = 0; i < 5; i++) {
        res = SherpaOnnxGetOnlineStreamResult(m_impl->recognizer, stream);
        if (res) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    if (res) {
        result.success = true;
        if (res->text) result.text = res->text;
        if (res->json) {
            auto metadata = parseSenseVoiceMetadata(res->json);
            result.emotion = metadata.emotion;
            result.event = metadata.event;
        }
        LOGI("Result: \"%s\"", result.text.c_str());
        LOGI("JSON: %s", res->json ? res->json : "null");
        SherpaOnnxDestroyOnlineRecognizerResult(res);
    } else {
        LOGI("Result is null");
    }
    
    SherpaOnnxDestroyOnlineStream(stream);
    return result;
}

bool SttEngine::isInitialized() const {
    return m_initialized;
}

BackendType SttEngine::backendType() const {
    return m_backendType;
}

const std::string& SttEngine::backendName() const {
    return m_backendName;
}

#endif

}
