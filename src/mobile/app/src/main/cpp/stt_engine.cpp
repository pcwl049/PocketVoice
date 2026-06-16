#include "stt_engine.h"
#include "qwen3_qnn_backend.h"
#include "qwen3_tokenizer.h"
#include "kaldi-native-fbank/csrc/online-feature.h"
#include "sherpa-onnx/csrc/math.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <thread>
#include <chrono>
#include <array>
#include <unistd.h>
#include <vector>
#ifndef STT_ENGINE_METADATA_ONLY
#include <android/log.h>
#endif

#ifndef STT_ENGINE_METADATA_ONLY
#define LOG_TAG "STT_Engine"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
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

namespace {

constexpr int kQwen3MelDim = 128;
constexpr int kQwen3MaxNewTokens = 128;
constexpr int kQwen3MaxTotalLen = 512;
constexpr int kQwen3ChunkSize = 100;
constexpr char kQwen3SystemPromptPrefix[] = "<|im_start|>system\n";
constexpr char kQwen3SystemPromptSuffix[] = "<|im_end|>\n<|im_start|>user\n<|audio_start|>";

static int featToAudioTokensLen(int featLen, int chunkSize) {
    if (featLen <= 0 || chunkSize <= 0) {
        return 0;
    }

    auto convOutLen3xStride2 = [](int n) -> int {
        int x = (n + 1) / 2;
        x = (x + 1) / 2;
        return (x + 1) / 2;
    };

    auto afterCnn = [](int x) -> int {
        if (x <= 0) {
            return 0;
        }
        x = (x - 1) / 2 + 1;
        x = (x - 1) / 2 + 1;
        return (x - 1) / 2 + 1;
    };

    const int cs = chunkSize;
    const int nChunks = (featLen + cs - 1) / cs;
    const int lastChunk = featLen - (nChunks - 1) * cs;
    return (nChunks - 1) * afterCnn(cs) + convOutLen3xStride2(lastChunk);
}

static std::vector<float> trimAudioFeaturesForDecoder(const std::vector<float>& audioFeatures,
                                                      int srcFrames,
                                                      int hiddenDim,
                                                      int keepFrames) {
    if (srcFrames <= 0 || hiddenDim <= 0 || keepFrames <= 0 || audioFeatures.empty()) {
        return {};
    }
    keepFrames = std::min(srcFrames, keepFrames);
    std::vector<float> trimmed(static_cast<size_t>(keepFrames) * hiddenDim, 0.0f);
    std::memcpy(trimmed.data(), audioFeatures.data(), trimmed.size() * sizeof(float));
    return trimmed;
}

static std::vector<float> extractQwen3WhisperFeatures(const float* samples, size_t numSamples, int* outFrames) {
    if (outFrames) {
        *outFrames = 0;
    }
    if (!samples || numSamples == 0) {
        return {};
    }

    knf::WhisperFeatureOptions whisperOpts;
    whisperOpts.frame_opts.samp_freq = 16000;
    whisperOpts.dim = kQwen3MelDim;
    knf::OnlineWhisperFbank whisperFbank(whisperOpts);
    whisperFbank.AcceptWaveform(16000.0f, samples, static_cast<int32_t>(numSamples));
    whisperFbank.InputFinished();

    const int numFrames = whisperFbank.NumFramesReady();
    if (numFrames < 2) {
        return {};
    }

    std::vector<float> features(static_cast<size_t>(numFrames) * kQwen3MelDim, 0.0f);
    float* dst = features.data();
    for (int i = 0; i < numFrames; ++i) {
        const float* frame = whisperFbank.GetFrame(i);
        std::copy(frame, frame + kQwen3MelDim, dst);
        dst += kQwen3MelDim;
    }

    sherpa_onnx::NormalizeWhisperFeatures(features.data(), numFrames, kQwen3MelDim);
    if (outFrames) {
        *outFrames = numFrames;
    }
    return features;
}

static std::vector<int64_t> buildQwen3Prompt(
    const Qwen3Tokenizer& tokenizer,
    int audioTokenLen,
    const std::string& hotwords,
    const std::string& language,
    int* outBeforeLen = nullptr,
    int* outAudioTokenLen = nullptr) {
    std::vector<int64_t> prompt;

    const std::string beforeUtf8 =
        std::string(kQwen3SystemPromptPrefix) + hotwords + kQwen3SystemPromptSuffix;
    const auto beforeIds = tokenizer.encode(beforeUtf8);
    prompt.insert(prompt.end(), beforeIds.begin(), beforeIds.end());

    const int beforeLen = static_cast<int>(beforeIds.size());
    if (outBeforeLen) {
        *outBeforeLen = beforeLen;
    }

    const auto audioPadIds = tokenizer.encode("<|audio_pad|>");
    const int oneAudioLen = static_cast<int>(audioPadIds.size());
    for (int i = 0; i < audioTokenLen; ++i) {
        prompt.insert(prompt.end(), audioPadIds.begin(), audioPadIds.end());
    }

    auto afterIds = tokenizer.encode("<|audio_end|><|im_end|>\n<|im_start|>assistant\n");
    prompt.insert(prompt.end(), afterIds.begin(), afterIds.end());

    if (!language.empty()) {
        auto languageIds = tokenizer.encode("language " + language);
        prompt.insert(prompt.end(), languageIds.begin(), languageIds.end());
        const int asrTextTokenId = tokenizer.getTokenId("<asr_text>");
        if (asrTextTokenId >= 0) {
            prompt.push_back(asrTextTokenId);
        }
    }

    // max_total_len 裁剪：与 sherpa 对齐，当 prompt 超过限制时裁剪 audio_pad 数量
    int contextLen = static_cast<int>(prompt.size());
    if (contextLen > kQwen3MaxTotalLen && oneAudioLen > 0) {
        const int afterLen = contextLen - beforeLen - audioTokenLen * oneAudioLen;
        const int keepAudio = std::max(0, (kQwen3MaxTotalLen - beforeLen - std::max(0, afterLen)) / oneAudioLen);
        if (keepAudio < audioTokenLen) {
            LOGW("[DIAG-prompt] context_len=%d exceeds max_total_len=%d, truncating audio_pad: %d -> %d",
                 contextLen, kQwen3MaxTotalLen, audioTokenLen, keepAudio);
            // Rebuild prompt with fewer audio pads
            std::vector<int64_t> truncated;
            truncated.reserve(beforeLen + keepAudio * oneAudioLen + std::max(0, afterLen));
            truncated.insert(truncated.end(), prompt.begin(), prompt.begin() + beforeLen);
            for (int i = 0; i < keepAudio; ++i) {
                truncated.insert(truncated.end(), audioPadIds.begin(), audioPadIds.end());
            }
            truncated.insert(truncated.end(), prompt.end() - std::max(0, afterLen), prompt.end());
            prompt = std::move(truncated);
            audioTokenLen = keepAudio;
        }
    }

    if (outAudioTokenLen) {
        *outAudioTokenLen = audioTokenLen;
    }

    return prompt;
}

static void removeUtf8ReplacementChars(std::string* text) {
    if (!text || text->empty()) {
        return;
    }

    const std::string replacement = "\xEF\xBF\xBD";
    size_t pos = 0;
    while ((pos = text->find(replacement, pos)) != std::string::npos) {
        text->erase(pos, replacement.size());
    }
}

static bool isQwen3StopToken(const Qwen3Tokenizer& tokenizer, int tokenId) {
    const int eosId = tokenizer.getEosTokenId();
    if (eosId >= 0 && tokenId == eosId) {
        return true;
    }

    static constexpr const char* kStopTokens[] = {
        "<|im_end|>",
        "<|endoftext|>",
        "<|audio_start|>",
        "<|audio_end|>",
        "<|audio_pad|>",
        "<|im_start|>",
    };
    for (const char* token : kStopTokens) {
        const int id = tokenizer.getTokenId(token);
        if (id >= 0 && tokenId == id) {
            return true;
        }
    }

    return false;
}

static int argmaxFromLogitsAvoiding(const std::vector<float>& logits, int avoidId) {
    int bestId = 0;
    float bestValue = -std::numeric_limits<float>::infinity();
    bool found = false;

    for (size_t i = 0; i < logits.size(); ++i) {
        if (avoidId >= 0 && static_cast<int>(i) == avoidId) {
            continue;
        }
        const float value = logits[i];
        if (std::isfinite(value) && value > bestValue) {
            bestValue = value;
            bestId = static_cast<int>(i);
            found = true;
        }
    }

    return found ? bestId : 0;
}

static std::vector<int64_t> cleanQwen3GeneratedIds(
    const Qwen3Tokenizer& tokenizer,
    const std::vector<int64_t>& generatedIds) {
    std::vector<int64_t> cleaned = generatedIds;
    if (generatedIds.empty()) {
        return cleaned;
    }

    const int asrTextTokenId = tokenizer.getTokenId("<asr_text>");
    if (asrTextTokenId < 0) {
        return cleaned;
    }

    const size_t prefixWindow = std::min<size_t>(16, generatedIds.size());
    auto asrTextIt = std::find(
        generatedIds.begin(),
        generatedIds.begin() + static_cast<std::ptrdiff_t>(prefixWindow),
        static_cast<int64_t>(asrTextTokenId));
    if (asrTextIt == generatedIds.begin() ||
        asrTextIt == generatedIds.begin() + static_cast<std::ptrdiff_t>(prefixWindow)) {
        return cleaned;
    }

    std::vector<int64_t> prefixIds(generatedIds.begin(), std::next(asrTextIt));
    const std::string prefixText = tokenizer.decode(prefixIds);
    if (prefixText.rfind("language ", 0) == 0 &&
        prefixText.size() >= 10 &&
        prefixText.compare(prefixText.size() - 10, 10, "<asr_text>") == 0) {
        cleaned.assign(std::next(asrTextIt), generatedIds.end());
    }

    return cleaned;
}

static std::string generateQwen3Text(
    Qwen3QnnBackend& backend,
    Qwen3Tokenizer& tokenizer,
    const std::vector<int64_t>& promptIds,
    const std::vector<float>& audioFeatures,
    std::vector<int>* generatedTokenIds) {
    if (promptIds.empty()) {
        return "";
    }

    const int contextLen = static_cast<int>(promptIds.size());
    const int maxSeqLen = kQwen3MaxTotalLen;

    std::vector<float> logits(151936, 0.0f);
    std::vector<int64_t> localGenerated;
    localGenerated.reserve(kQwen3MaxNewTokens);

    // [DIAG-prompt] Log prompt tokens
    LOGI("[DIAG-prompt] prompt length: %d tokens (max_total_len=%d)", contextLen, maxSeqLen);
    for (size_t i = 0; i < std::min<size_t>(20, promptIds.size()); ++i) {
        LOGI("[DIAG-prompt] token[%zu] = %lld", i, static_cast<long long>(promptIds[i]));
    }
    if (promptIds.size() > 20) {
        LOGI("[DIAG-prompt] ... (%zu more tokens)", promptIds.size() - 20);
    }

    // Prompt prefill: token-by-token (required by W=4 fixed-window decoder)
    int currentToken = static_cast<int>(promptIds.front());
    for (size_t i = 1; i < promptIds.size(); ++i) {
        if (!backend.decodeStep(currentToken, audioFeatures.data(), audioFeatures.size(), logits.data())) {
            LOGE("Qwen3 prompt prefill failed at token index %zu", i - 1);
            break;
        }
        currentToken = static_cast<int>(promptIds[i]);
    }

    const int endOfTextId = tokenizer.getTokenId("<|endoftext|>");
    const int imEndId = tokenizer.getTokenId("<|im_end|>");
    LOGI("[DIAG-gen] stop ids: eos=%d im_end=%d endoftext=%d",
         tokenizer.getEosTokenId(), imEndId, endOfTextId);

    int curLen = contextLen;

    for (int step = 0; step < kQwen3MaxNewTokens; ++step) {
        // sherpa-aligned: check total sequence length limit
        if (curLen >= maxSeqLen) {
            LOGI("[DIAG-gen] reached max_total_len=%d at step=%d, stopping", maxSeqLen, step);
            break;
        }

        if (step + 1 == kQwen3MaxNewTokens) {
            LOGW("[DIAG-gen] result truncated, max_new_tokens=%d reached", kQwen3MaxNewTokens);
        }

        if (!backend.decodeStep(currentToken, audioFeatures.data(), audioFeatures.size(), logits.data())) {
            LOGE("Qwen3 generation failed at step %d", step);
            break;
        }

        const int nextId = backend.getLastArgmax();
        int selectedId = nextId;
        const int eosId = tokenizer.getEosTokenId();
        if (step == 0 && eosId >= 0 && selectedId == eosId) {
            selectedId = argmaxFromLogitsAvoiding(logits, eosId);
            LOGI("[DIAG-gen] first token was eos=%d, retry selected=%d", eosId, selectedId);
        }

        if (step < 20) {
            LOGI("[DIAG-gen] step=%d, input=%d, argmax=%d, selected=%d, logits_sum=%.2f",
                 step, currentToken, nextId, selectedId, backend.getLastLogitsSum());
        }

        if (isQwen3StopToken(tokenizer, selectedId)) {
            LOGI("[DIAG-gen] stop at step=%d token=%d", step, selectedId);
            break;
        }

        localGenerated.push_back(selectedId);
        currentToken = selectedId;
        ++curLen;
    }

    if (generatedTokenIds) {
        generatedTokenIds->clear();
        generatedTokenIds->reserve(localGenerated.size());
        for (int64_t id : localGenerated) {
            generatedTokenIds->push_back(static_cast<int>(id));
        }
    }

    const std::vector<int64_t> cleanedIds = cleanQwen3GeneratedIds(tokenizer, localGenerated);
    std::string text = tokenizer.decode(cleanedIds);
    removeUtf8ReplacementChars(&text);
    LOGI("[DIAG-gen] generated=%zu cleaned=%zu text_len=%zu stop_reason=%s",
         localGenerated.size(), cleanedIds.size(), text.size(),
         curLen >= maxSeqLen ? "max_total_len" :
         (localGenerated.size() >= static_cast<size_t>(kQwen3MaxNewTokens) ? "max_new_tokens" : "stop_token"));
    return text;
}

}  // namespace

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
    stt::Qwen3QnnBackend* qwen3QnnBackend = nullptr;
    stt::Qwen3Tokenizer* qwen3Tokenizer = nullptr;
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

static std::string readTextFile(const std::string& path) {
    std::ifstream is(path, std::ios::binary);
    if (!is) return "";
    std::string value((std::istreambuf_iterator<char>(is)), std::istreambuf_iterator<char>());
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r' || value.back() == ' ' || value.back() == '\t')) {
        value.pop_back();
    }
    return value;
}

static bool endsWith(const std::string& value, const std::string& suffix) {
    return value.size() >= suffix.size() &&
           value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
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

static long readRssKb() {
    std::ifstream is("/proc/self/statm");
    if (!is) return 0;

    long sizePages = 0;
    long residentPages = 0;
    is >> sizePages >> residentPages;
    if (residentPages <= 0) return 0;

    const long pageSize = static_cast<long>(sysconf(_SC_PAGESIZE));
    if (pageSize <= 0) return 0;

    return residentPages * (pageSize / 1024);
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
    if (m_impl->qwen3QnnBackend) {
        delete m_impl->qwen3QnnBackend;
    }
    if (m_impl->qwen3Tokenizer) {
        delete m_impl->qwen3Tokenizer;
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
    std::string qwen3ConvFrontendPath = modelDir + "/conv_frontend.onnx";
    std::string qwen3TokenizerPath = modelDir + "/tokenizer";
    std::string qwen3HotwordsPath = modelDir + "/qwen3_hotwords.txt";
    
    std::string qwen3CpuModelDir = modelDir;
    if (endsWith(modelDir, "-qnn")) {
        qwen3CpuModelDir = modelDir.substr(0, modelDir.size() - 4);
    }

    std::string qwen3QnnModelDir = endsWith(modelDir, "-qnn") ? modelDir : (modelDir + "-qnn");
    // Prefer decoder-w128 if present, fallback to decoder-w4
    std::string qwen3QnnDecoderDir = qwen3QnnModelDir + "/decoder-w128";
    std::string qwen3QnnDecoderLibPath = qwen3QnnDecoderDir + "/libmodel.so";
    if (!fileExists(qwen3QnnDecoderLibPath)) {
        qwen3QnnDecoderDir = qwen3QnnModelDir + "/decoder-w4";
        qwen3QnnDecoderLibPath = qwen3QnnDecoderDir + "/libmodel.so";
    }
    LOGI("  QNN decoder window: %s", qwen3QnnDecoderDir.c_str());
    std::string qwen3QnnRuntimeDecoderLibPath = qnnLibDir.empty() ? "" : (qnnLibDir + "/libmodel.so");
    std::string qwen3QnnConvFrontendPath = qwen3QnnModelDir + "/conv_frontend/libmodel.so";
    std::string qwen3QnnEncoderPath = qwen3QnnModelDir + "/encoder/libmodel.so";
    std::string qwen3QnnTokenizerPath = qwen3QnnModelDir + "/tokenizer";
    std::string qwen3CpuConvFrontendPath = qwen3CpuModelDir + "/conv_frontend.onnx";
    std::string qwen3CpuEncoderPath = qwen3CpuModelDir + "/encoder.int8.onnx";
    std::string qwen3CpuDecoderPath = qwen3CpuModelDir + "/decoder.int8.onnx";
    std::string qwen3CpuTokenizerPath = qwen3CpuModelDir + "/tokenizer";
    std::string qwen3CpuHotwordsPath = qwen3CpuModelDir + "/qwen3_hotwords.txt";
    
    LOGI("Check files...");
    LOGI("  model dir: %s", modelDir.c_str());
    LOGI("  sensevoice qnn model.bin: %s", fileExists(senseVoiceQnnPath) ? "exists" : "MISSING");
    LOGI("  sensevoice qnn libmodel.so: %s", fileExists(senseVoiceQnnLibPath) ? "exists" : "MISSING");
    LOGI("  zipformer model.int8.onnx: %s", fileExists(zipformerPath) ? "exists" : "MISSING");
    LOGI("  zipformer bbpe.model: %s", fileExists(bbpePath) ? "exists" : "MISSING");
    LOGI("  tokens.txt: %s", fileExists(tokensPath) ? "exists" : "MISSING");
    LOGI("  paraformer encoder.int8.onnx: %s", fileExists(encoderPath) ? "exists" : "MISSING");
    LOGI("  paraformer decoder.int8.onnx: %s", fileExists(decoderPath) ? "exists" : "MISSING");
    LOGI("  qwen3 conv_frontend.onnx: %s", fileExists(qwen3ConvFrontendPath) ? "exists" : "MISSING");
    LOGI("  qwen3 tokenizer: %s", fileExists(qwen3TokenizerPath) ? "exists" : "MISSING");
    LOGI("  qwen3 qnn model dir: %s", fileExists(qwen3QnnModelDir) ? "exists" : "MISSING");
    LOGI("  qwen3 qnn decoder lib: %s", fileExists(qwen3QnnDecoderLibPath) ? "exists" : "MISSING");
    LOGI("  qwen3 qnn runtime decoder lib: %s", (!qwen3QnnRuntimeDecoderLibPath.empty() && fileExists(qwen3QnnRuntimeDecoderLibPath)) ? "exists" : "MISSING");
    LOGI("  qwen3 cpu model dir: %s", qwen3CpuModelDir.c_str());
    LOGI("  paraformer qnn libencoder.so: %s", fileExists(modelDir + "/libencoder.so") ? "exists" : "MISSING");
    LOGI("  paraformer qnn libpredictor.so: %s", fileExists(modelDir + "/libpredictor.so") ? "exists" : "MISSING");
    LOGI("  paraformer qnn libdecoder.so: %s", fileExists(modelDir + "/libdecoder.so") ? "exists" : "MISSING");

    bool hasSenseVoiceContext = fileExists(senseVoiceQnnPath);
    bool hasSenseVoiceLib = fileExists(senseVoiceQnnLibPath);
    bool useSenseVoiceQnn = (hasSenseVoiceContext || hasSenseVoiceLib) && fileExists(tokensPath);
    bool useZipformer = fileExists(zipformerPath) && fileExists(tokensPath) && fileExists(bbpePath);
    bool useParaformer = fileExists(encoderPath) && fileExists(decoderPath) && fileExists(tokensPath);
    bool useQwen3Asr = fileExists(qwen3CpuConvFrontendPath) && fileExists(qwen3CpuEncoderPath) && fileExists(qwen3CpuDecoderPath) && fileExists(qwen3CpuTokenizerPath);
    bool useQwen3AsrQnn = (fileExists(qwen3QnnDecoderLibPath) || (!qwen3QnnRuntimeDecoderLibPath.empty() && fileExists(qwen3QnnRuntimeDecoderLibPath))) &&
        fileExists(qwen3QnnConvFrontendPath) && fileExists(qwen3QnnEncoderPath) && fileExists(qwen3QnnTokenizerPath);

    // Paraformer QNN: detect libencoder.so + libpredictor.so + libdecoder.so + tokens.txt
    std::string paraformerQnnEncoderLib = modelDir + "/libencoder.so";
    std::string paraformerQnnPredictorLib = modelDir + "/libpredictor.so";
    std::string paraformerQnnDecoderLib = modelDir + "/libdecoder.so";
    bool useParaformerQnn = fileExists(paraformerQnnEncoderLib)
        && fileExists(paraformerQnnPredictorLib)
        && fileExists(paraformerQnnDecoderLib)
        && fileExists(tokensPath);

    // Paraformer XNNPACK: offline Paraformer with XNNPACK EP
    std::string paraformerOfflineModelInt8 = modelDir + "/model.int8.onnx";
    std::string paraformerOfflineModelFp32 = modelDir + "/model.onnx";
    std::string paraformerOfflineModel;
    if (fileExists(paraformerOfflineModelInt8)) {
        paraformerOfflineModel = paraformerOfflineModelInt8;
    } else if (fileExists(paraformerOfflineModelFp32)) {
        paraformerOfflineModel = paraformerOfflineModelFp32;
    }
    bool useParaformerXnnpack = !paraformerOfflineModel.empty() && fileExists(tokensPath);

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

    if (useParaformerQnn) {
        m_backendType = BackendType::ParaformerQnn;
        m_backendName = "paraformer_qnn";

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
        }

        // Paraformer QNN: model field is comma-separated libencoder.so,libpredictor.so,libdecoder.so
        std::string paraformerQnnModelPaths = paraformerQnnEncoderLib + "," + paraformerQnnPredictorLib + "," + paraformerQnnDecoderLib;

        SherpaOnnxOfflineRecognizerConfig config;
        memset(&config, 0, sizeof(config));
        config.feat_config.sample_rate = 16000;
        config.feat_config.feature_dim = 80;
        config.model_config.paraformer.model = paraformerQnnModelPaths.c_str();
        config.model_config.paraformer.qnn_backend_lib = qnnBackendLib.c_str();
        config.model_config.paraformer.qnn_context_binary = "";
        config.model_config.paraformer.qnn_system_lib = "";
        config.model_config.tokens = tokensPath.c_str();
        config.model_config.num_threads = 1;
        config.model_config.provider = "qnn";
        config.model_config.debug = 0;
        config.decoding_method = "greedy_search";

        LOGI("Selected backend: %s", m_backendName.c_str());
        LOGI("Paraformer QNN model paths: %s", paraformerQnnModelPaths.c_str());
        LOGI("QNN backend lib: %s", qnnBackendLib.c_str());
        LOGI("QNN system lib: %s", qnnSystemLib.c_str());
        LOGI("Creating offline Paraformer QNN recognizer...");
        m_impl->offlineRecognizer = SherpaOnnxCreateOfflineRecognizer(&config);
        if (!m_impl->offlineRecognizer) {
            LOGE("Failed to create offline Paraformer QNN recognizer");
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

    if (useParaformerXnnpack) {
        m_backendType = BackendType::ParaformerXnnpack;
        m_backendName = "paraformer_xnnpack";

        const int cpuFallbackThreads = readCpuFallbackThreads(modelDir);

        SherpaOnnxOfflineRecognizerConfig config;
        memset(&config, 0, sizeof(config));
        config.model_config.paraformer.model = paraformerOfflineModel.c_str();
        config.model_config.model_type = "paraformer";
        config.model_config.provider = "xnnpack";
        config.model_config.num_threads = cpuFallbackThreads;
        config.feat_config.sample_rate = 16000;
        config.feat_config.feature_dim = 80;
        config.decoding_method = "greedy_search";

        LOGI("Selected backend: %s", m_backendName.c_str());
        LOGI("Paraformer XNNPACK model: %s", paraformerOfflineModel.c_str());
        LOGI("CPU fallback threads: %d", cpuFallbackThreads);
        LOGI("Creating offline Paraformer XNNPACK recognizer...");
        m_impl->offlineRecognizer = SherpaOnnxCreateOfflineRecognizer(&config);
        if (!m_impl->offlineRecognizer) {
            LOGE("Failed to create Paraformer XNNPACK recognizer, falling back to CPU");
            config.model_config.provider = "cpu";
            m_impl->offlineRecognizer = SherpaOnnxCreateOfflineRecognizer(&config);
            if (!m_impl->offlineRecognizer) {
                LOGE("Failed to create Paraformer CPU fallback recognizer");
                return false;
            }
            m_backendName = "paraformer_cpu";
            LOGI("Using CPU provider for Paraformer (XNNPACK not available)");
        } else {
            LOGI("Paraformer XNNPACK recognizer created successfully");
        }
        m_initialized = true;
        LOGI("Initialized OK, backend=%s", m_backendName.c_str());
        return true;
    }

#if STT_USE_QNN
    if (useQwen3AsrQnn) {
        m_backendType = BackendType::Qwen3AsrQnn;
        m_backendName = "qwen3_asr_qnn";

        LOGI("Selected backend: %s", m_backendName.c_str());
        LOGI("Qwen3 QNN decoder lib: %s", qwen3QnnDecoderLibPath.c_str());
        LOGI("Qwen3 QNN runtime dir: %s", qnnLibDir.c_str());
        LOGI("Qwen3 QNN conv frontend lib: %s", qwen3QnnConvFrontendPath.c_str());
        LOGI("Qwen3 QNN encoder lib: %s", qwen3QnnEncoderPath.c_str());
        LOGI("Qwen3 QNN tokenizer: %s", qwen3QnnTokenizerPath.c_str());
        
        // Initialize Qwen3 QNN backend
        m_impl->qwen3QnnBackend = new stt::Qwen3QnnBackend();
        if (!m_impl->qwen3QnnBackend->init(qwen3QnnModelDir, qnnLibDir)) {
            LOGE("Failed to initialize Qwen3 QNN backend");
            delete m_impl->qwen3QnnBackend;
            m_impl->qwen3QnnBackend = nullptr;
            
            // Fall back to CPU
            LOGI("Falling back to Qwen3 CPU backend");
            m_backendType = BackendType::Qwen3AsrCpu;
            m_backendName = "qwen3_asr_cpu";
            
            // Try CPU fallback
            if (useQwen3Asr) {
                LOGI("Attempting CPU fallback...");
                // CPU fallback will be handled below
            } else {
                LOGE("No CPU fallback available");
                return false;
            }
        } else {
            // Initialize tokenizer
            m_impl->qwen3Tokenizer = new stt::Qwen3Tokenizer();
            if (!m_impl->qwen3Tokenizer->load(qwen3QnnTokenizerPath)) {
                LOGE("Failed to load tokenizer");
                delete m_impl->qwen3Tokenizer;
                m_impl->qwen3Tokenizer = nullptr;
                // Continue without tokenizer for now
            }
            
            // Gate A: Runtime Encoding Audit - log HTP tensor encodings after finalize
            LOGI("Running runtime encoding audit...");
            m_impl->qwen3QnnBackend->auditRuntimeEncodings();

            // Run decoder smoke test to verify numeric alignment
            LOGI("Running decoder smoke test...");
            if (m_impl->qwen3QnnBackend->runDecoderSmokeTest()) {
                LOGI("Decoder smoke test passed");
            } else {
                LOGW("Decoder smoke test failed - check logs for details");
                // Continue anyway for now, but log the failure
            }

            // Gate 1: KV Influence Probe - prove non-zero KV changes logits
            LOGI("Running KV influence probe...");
            if (m_impl->qwen3QnnBackend->runKvInfluenceProbe()) {
                LOGI("KV influence probe PASSED - non-zero KV changes logits");
            } else {
                LOGE("KV influence probe FAILED - KV has no effect on output!");
            }
            
            m_initialized = true;
            LOGI("Initialized OK, backend=%s", m_backendName.c_str());
            return true;
        }
    }
#endif

#if STT_USE_QNN
    if (useQwen3Asr) {
        m_backendType = BackendType::Qwen3AsrCpu;
        m_backendName = "qwen3_asr_cpu";

        std::string hotwords = readTextFile(qwen3CpuHotwordsPath);
        const int cpuFallbackThreads = readCpuFallbackThreads(qwen3CpuModelDir);

        SherpaOnnxOfflineRecognizerConfig config;
        memset(&config, 0, sizeof(config));
        config.feat_config.sample_rate = 16000;
        config.feat_config.feature_dim = 128;
        config.model_config.qwen3_asr.conv_frontend = qwen3CpuConvFrontendPath.c_str();
        config.model_config.qwen3_asr.encoder = qwen3CpuEncoderPath.c_str();
        config.model_config.qwen3_asr.decoder = qwen3CpuDecoderPath.c_str();
        config.model_config.qwen3_asr.tokenizer = qwen3CpuTokenizerPath.c_str();
        config.model_config.qwen3_asr.max_total_len = 512;
        config.model_config.qwen3_asr.max_new_tokens = 128;
        config.model_config.qwen3_asr.temperature = 0.000001f;
        config.model_config.qwen3_asr.top_p = 0.8f;
        config.model_config.qwen3_asr.seed = 42;
        config.model_config.qwen3_asr.hotwords = hotwords.empty() ? nullptr : hotwords.c_str();
        config.model_config.num_threads = cpuFallbackThreads;
        config.model_config.provider = "cpu";
        config.model_config.debug = 0;
        config.decoding_method = "greedy_search";

        LOGI("Selected backend: %s", m_backendName.c_str());
        LOGI("CPU fallback threads: %d", cpuFallbackThreads);
        LOGI("Qwen3 hotwords: %s", hotwords.empty() ? "empty" : "configured");
        LOGI("Creating offline Qwen3-ASR recognizer...");

        __android_log_print(ANDROID_LOG_INFO, "STT_GATE_D1", "backend=%s", m_backendName.c_str());
        __android_log_print(ANDROID_LOG_INFO, "STT_GATE_D1", "init_start");
        const auto initStart = std::chrono::steady_clock::now();
        m_impl->offlineRecognizer = SherpaOnnxCreateOfflineRecognizer(&config);
        const auto initEnd = std::chrono::steady_clock::now();
        const long initMs = static_cast<long>(
            std::chrono::duration_cast<std::chrono::milliseconds>(initEnd - initStart).count());
        const long initRssKb = readRssKb();

        if (!m_impl->offlineRecognizer) {
            __android_log_print(ANDROID_LOG_ERROR, "STT_GATE_D1",
                "init_failed init_ms=%ld rss_kb=%ld", initMs, initRssKb);
            LOGE("Failed to create offline Qwen3-ASR recognizer");
            return false;
        }

        __android_log_print(ANDROID_LOG_INFO, "STT_GATE_D1",
            "init_done init_ms=%ld rss_kb=%ld", initMs, initRssKb);
        m_initialized = true;
        LOGI("Initialized OK, backend=%s", m_backendName.c_str());
        return true;
    }
#else
    if (useQwen3Asr) {
        LOGE("Qwen3-ASR requires the default QNN APK build because the CPU library bundle is older than the Qwen3 C API");
        return false;
    }
#endif

    if (!useZipformer && !useParaformer && !useParaformerXnnpack) {
        LOGE("No supported model set found in %s", modelDir.c_str());
        LOGE("Expected SenseVoice QNN: model.bin/libmodel.so + tokens.txt");
        LOGE("Expected Zipformer CTC: model.int8.onnx + bbpe.model + tokens.txt");
        LOGE("Expected Paraformer fallback: encoder.int8.onnx + decoder.int8.onnx + tokens.txt");
        LOGE("Expected Paraformer QNN: libencoder.so + libpredictor.so + libdecoder.so + tokens.txt");
        LOGE("Expected Paraformer XNNPACK: model.int8.onnx or model.onnx + tokens.txt");
#if STT_USE_QNN
        LOGE("Expected Qwen3-ASR CPU: conv_frontend.onnx + encoder.int8.onnx + decoder.int8.onnx + tokenizer/");
#endif
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

    if (m_backendType == BackendType::ParaformerQnn) {
        if (!m_impl->offlineRecognizer) return result;

        const SherpaOnnxOfflineStream* stream = SherpaOnnxCreateOfflineStream(m_impl->offlineRecognizer);
        if (!stream) return result;

        SherpaOnnxAcceptWaveformOffline(stream, 16000, samples, static_cast<int32_t>(numSamples));

        const auto decodeStart = std::chrono::steady_clock::now();
        SherpaOnnxDecodeOfflineStream(m_impl->offlineRecognizer, stream);
        const auto decodeEnd = std::chrono::steady_clock::now();
        const long decodeMs = static_cast<long>(
            std::chrono::duration_cast<std::chrono::milliseconds>(decodeEnd - decodeStart).count());
        const long audioMs = static_cast<long>(numSamples * 1000 / 16000);

        const SherpaOnnxOfflineRecognizerResult* res = SherpaOnnxGetOfflineStreamResult(stream);

        if (res) {
            result.success = true;
            if (res->text) result.text = res->text;
            LOGI("Result: \"%s\"", result.text.c_str());
            LOGI("Decode time: %ld ms", decodeMs);
            __android_log_print(ANDROID_LOG_INFO, "STT_GATE_D3",
                "backend=%s decode_ms=%ld audio_ms=%ld", m_backendName.c_str(), decodeMs, audioMs);
            SherpaOnnxDestroyOfflineRecognizerResult(res);
        } else {
            LOGI("Result is null");
        }
        SherpaOnnxDestroyOfflineStream(stream);
        return result;
    }
#endif

    if (m_backendType == BackendType::ParaformerXnnpack) {
        if (!m_impl->offlineRecognizer) return result;

        const SherpaOnnxOfflineStream* stream = SherpaOnnxCreateOfflineStream(m_impl->offlineRecognizer);
        if (!stream) return result;

        SherpaOnnxAcceptWaveformOffline(stream, 16000, samples, static_cast<int32_t>(numSamples));
        SherpaOnnxDecodeOfflineStream(m_impl->offlineRecognizer, stream);

        const SherpaOnnxOfflineRecognizerResult* res = SherpaOnnxGetOfflineStreamResult(stream);
        if (res) {
            result.success = true;
            if (res->text) result.text = res->text;
            LOGI("Result: \"%s\"", result.text.c_str());
            SherpaOnnxDestroyOfflineRecognizerResult(res);
        } else {
            LOGI("Result is null");
        }
        SherpaOnnxDestroyOfflineStream(stream);
        return result;
    }

    if (m_backendType == BackendType::Qwen3AsrCpu) {
        if (!m_impl->offlineRecognizer) return result;

        const long audioDurationMs = static_cast<long>(numSamples * 1000 / 16000);
        __android_log_print(ANDROID_LOG_INFO, "STT_GATE_D1",
            "audio_duration_ms=%ld audio_samples=%zu", audioDurationMs, numSamples);

        const SherpaOnnxOfflineStream* stream = SherpaOnnxCreateOfflineStream(m_impl->offlineRecognizer);
        if (!stream) {
            __android_log_print(ANDROID_LOG_ERROR, "STT_GATE_D1", "stream_create_failed");
            return result;
        }

        SherpaOnnxAcceptWaveformOffline(stream, 16000, samples, static_cast<int32_t>(numSamples));

        const long rssBefore = readRssKb();
        __android_log_print(ANDROID_LOG_INFO, "STT_GATE_D1",
            "decode_start rss_before_kb=%ld", rssBefore);

        const auto decodeStart = std::chrono::steady_clock::now();
        SherpaOnnxDecodeOfflineStream(m_impl->offlineRecognizer, stream);
        const auto decodeEnd = std::chrono::steady_clock::now();
        const long decodeMs = static_cast<long>(
            std::chrono::duration_cast<std::chrono::milliseconds>(decodeEnd - decodeStart).count());

        const long rssAfter = readRssKb();
        __android_log_print(ANDROID_LOG_INFO, "STT_GATE_D1",
            "decode_done total_ms=%ld rss_after_kb=%ld rss_delta_kb=%ld",
            decodeMs, rssAfter, rssAfter - rssBefore);

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

    if (m_backendType == BackendType::Qwen3AsrQnn) {
        if (!m_impl->qwen3QnnBackend || !m_impl->qwen3QnnBackend->isInitialized()) {
            LOGE("Qwen3 QNN backend not initialized");
            return result;
        }
        if (!m_impl->qwen3Tokenizer || !m_impl->qwen3Tokenizer->isLoaded()) {
            LOGE("Qwen3 tokenizer not initialized");
            return result;
        }
        
        LOGI("Qwen3 QNN recognition started");

        int featFrames = 0;
        const std::vector<float> whisperFeatures = extractQwen3WhisperFeatures(samples, numSamples, &featFrames);
        if (whisperFeatures.empty() || featFrames < 2) {
            LOGE("Whisper feature extraction failed");
            return result;
        }

        std::vector<float> convOutput;
        int convFrames = 0;
        if (!m_impl->qwen3QnnBackend->runConvFrontend(
                whisperFeatures.data(), featFrames, &convOutput, &convFrames)) {
            LOGE("Conv frontend failed");
            return result;
        }
        LOGI("Conv frontend completed");

        const int expectedAudioTokenLen = featToAudioTokensLen(featFrames, kQwen3ChunkSize);
        const int decoderFrameCapacity = m_impl->qwen3QnnBackend->getAudioFeatureFrameCapacity();
        const int validAudioTokens = std::min({expectedAudioTokenLen, convFrames, decoderFrameCapacity > 0 ? decoderFrameCapacity : convFrames});
        std::vector<float> audioFeatures;
        if (!m_impl->qwen3QnnBackend->runEncoder(
                convOutput.data(), convFrames, validAudioTokens, &audioFeatures)) {
            LOGE("Encoder failed");
            return result;
        }
        LOGI("Encoder completed");

        const int encoderOutputFrames = convFrames;
        std::vector<float> decoderAudioFeatures = trimAudioFeaturesForDecoder(
            audioFeatures, encoderOutputFrames, 1024, validAudioTokens);
        if (decoderAudioFeatures.empty()) {
            LOGE("Decoder audio feature trim failed");
            return result;
        }

        // [DIAG-audio-features] Log trimmed audio features range
        {
            float minVal = std::numeric_limits<float>::max();
            float maxVal = std::numeric_limits<float>::lowest();
            float sum = 0.0f;
            size_t nonzero = 0;
            for (size_t i = 0; i < decoderAudioFeatures.size(); ++i) {
                minVal = std::min(minVal, decoderAudioFeatures[i]);
                maxVal = std::max(maxVal, decoderAudioFeatures[i]);
                sum += decoderAudioFeatures[i];
                if (decoderAudioFeatures[i] != 0.0f) ++nonzero;
            }
            LOGI("[DIAG-audio-features] decoder input: count=%zu, min=%.6f, max=%.6f, sum=%.2f, nonzero=%zu",
                 decoderAudioFeatures.size(), minVal, maxVal, sum, nonzero);
        }

        m_impl->qwen3QnnBackend->resetState();

        int promptAudioTokenLen = 0;
        const auto promptIds = buildQwen3Prompt(
            *m_impl->qwen3Tokenizer,
            validAudioTokens,
            "",
            "",
            nullptr,
            &promptAudioTokenLen);

        // 如果 prompt 裁剪了 audio_pad，也裁剪 decoder 的 audio features
        if (promptAudioTokenLen < validAudioTokens && promptAudioTokenLen > 0) {
            LOGI("[DIAG] audio features trimmed to match prompt: %d -> %d", validAudioTokens, promptAudioTokenLen);
            decoderAudioFeatures = trimAudioFeaturesForDecoder(
                audioFeatures, encoderOutputFrames, 1024, promptAudioTokenLen);
            if (decoderAudioFeatures.empty()) {
                LOGE("Decoder audio feature re-trim failed after prompt truncation");
                return result;
            }
        }

        std::vector<int> generatedTokens;
        result.text = generateQwen3Text(
            *m_impl->qwen3QnnBackend,
            *m_impl->qwen3Tokenizer,
            promptIds,
            decoderAudioFeatures,
            &generatedTokens);

        LOGI("Decoded text: %s", result.text.c_str());
        result.success = true;

        // Gate 3: dump per-step diagnostic records
        m_impl->qwen3QnnBackend->dumpDiagRecords();

        LOGI("Qwen3 QNN recognition completed");
        return result;
    }

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
