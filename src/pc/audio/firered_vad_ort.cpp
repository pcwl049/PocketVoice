#include "firered_vad_ort.h"
#include "../pc_logger.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>

#include "onnxruntime_c_api.h"
#include "frontend/fbank.h"

namespace stt {

namespace {

constexpr int kSampleRate = 16000;
constexpr int kFrameShiftSamples = 160;
constexpr int kFrameLengthSamples = 400;
constexpr int kFeatDim = 80;
constexpr int64_t kCacheSize = 1024;
constexpr int64_t kCacheLen = 19;

static bool fileExists(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec);
}

static int16_t toPcm16(float sample) {
    float clipped = std::max(-1.0f, std::min(1.0f, sample));
    return static_cast<int16_t>(std::lrintf(clipped * 32767.0f));
}

static double readDoubleLE(const std::vector<uint8_t>& data, size_t offset) {
    double value = 0.0;
    std::memcpy(&value, data.data() + offset, sizeof(value));
    return value;
}

static int32_t readI32LE(const std::vector<uint8_t>& data, size_t offset) {
    int32_t value = 0;
    std::memcpy(&value, data.data() + offset, sizeof(value));
    return value;
}

static bool loadKaldiCmvn(const std::filesystem::path& path,
                          std::vector<float>& means,
                          std::vector<float>& inverseStd) {
    std::ifstream is(path, std::ios::binary);
    if (!is) return false;

    std::vector<uint8_t> data((std::istreambuf_iterator<char>(is)), std::istreambuf_iterator<char>());
    if (data.size() < 15 || data[0] != 0 || data[1] != 'B' || data[2] != 'D' ||
        data[3] != 'M' || data[4] != ' ') {
        return false;
    }

    size_t pos = 5;
    if (data[pos++] != 4) return false;
    int32_t rows = readI32LE(data, pos);
    pos += 4;
    if (data[pos++] != 4) return false;
    int32_t cols = readI32LE(data, pos);
    pos += 4;
    if (rows != 2 || cols != kFeatDim + 1) return false;
    if (pos + static_cast<size_t>(rows) * static_cast<size_t>(cols) * sizeof(double) > data.size()) return false;

    std::vector<double> stats(static_cast<size_t>(rows) * static_cast<size_t>(cols));
    for (size_t i = 0; i < stats.size(); ++i) {
        stats[i] = readDoubleLE(data, pos + i * sizeof(double));
    }

    double count = stats[kFeatDim];
    if (count < 1.0) return false;

    means.resize(kFeatDim);
    inverseStd.resize(kFeatDim);
    for (int d = 0; d < kFeatDim; ++d) {
        double mean = stats[d] / count;
        double variance = stats[cols + d] / count - mean * mean;
        if (variance < 1e-20) variance = 1e-20;
        means[d] = static_cast<float>(mean);
        inverseStd[d] = static_cast<float>(1.0 / std::sqrt(variance));
    }
    return true;
}

static void applyCmvn(const std::vector<float>& means,
                      const std::vector<float>& inverseStd,
                      std::vector<float>& features) {
    if (means.size() != kFeatDim || inverseStd.size() != kFeatDim || features.size() < kFeatDim) return;
    for (int d = 0; d < kFeatDim; ++d) {
        features[d] = (features[d] - means[d]) * inverseStd[d];
    }
}

static std::wstring widenPath(const std::filesystem::path& path) {
    return path.wstring();
}

}  // namespace

struct FireRedVadOrt::Impl {
    const OrtApi* api = nullptr;
    OrtEnv* env = nullptr;
    OrtSessionOptions* sessionOptions = nullptr;
    OrtSession* session = nullptr;
    OrtMemoryInfo* memoryInfo = nullptr;
    OrtAllocator* allocator = nullptr;
    std::vector<std::string> inputNames;
    std::vector<std::string> outputNames;
    std::vector<const char*> inputNamePtrs;
    std::vector<const char*> outputNamePtrs;
    std::vector<float> cache;
    std::vector<float> cmvnMeans;
    std::vector<float> cmvnIstd;
    std::deque<float> audioWindow;
    std::vector<float> pending;
    std::unique_ptr<vad::Fbank> fbank;
    float threshold = 0.4f;

    void release() {
        if (api) {
            if (session) api->ReleaseSession(session);
            if (sessionOptions) api->ReleaseSessionOptions(sessionOptions);
            if (memoryInfo) api->ReleaseMemoryInfo(memoryInfo);
            if (env) api->ReleaseEnv(env);
        }
        session = nullptr;
        sessionOptions = nullptr;
        memoryInfo = nullptr;
        env = nullptr;
        allocator = nullptr;
    }

    bool check(OrtStatus* status, const char* step) {
        if (!status) return true;
        const char* message = api ? api->GetErrorMessage(status) : "unknown";
        pcLogf(PcLogLevel::Error, "VAD", "FireRedVAD %s failed: %s", step, message ? message : "unknown");
        if (api) api->ReleaseStatus(status);
        return false;
    }

    bool collectNames() {
        size_t inputCount = 0;
        size_t outputCount = 0;
        if (!check(api->SessionGetInputCount(session, &inputCount), "get input count")) return false;
        if (!check(api->SessionGetOutputCount(session, &outputCount), "get output count")) return false;
        if (inputCount != 2 || outputCount != 2) {
            pcLogf(PcLogLevel::Error, "VAD", "FireRedVAD expected 2 inputs and 2 outputs, got %zu/%zu", inputCount, outputCount);
            return false;
        }
        if (!check(api->GetAllocatorWithDefaultOptions(&allocator), "get allocator")) return false;

        inputNames.clear();
        outputNames.clear();
        for (size_t i = 0; i < inputCount; ++i) {
            char* raw = nullptr;
            if (!check(api->SessionGetInputName(session, i, allocator, &raw), "get input name")) return false;
            inputNames.emplace_back(raw ? raw : "");
            api->AllocatorFree(allocator, raw);
        }
        for (size_t i = 0; i < outputCount; ++i) {
            char* raw = nullptr;
            if (!check(api->SessionGetOutputName(session, i, allocator, &raw), "get output name")) return false;
            outputNames.emplace_back(raw ? raw : "");
            api->AllocatorFree(allocator, raw);
        }
        inputNamePtrs.clear();
        outputNamePtrs.clear();
        for (const auto& name : inputNames) inputNamePtrs.push_back(name.c_str());
        for (const auto& name : outputNames) outputNamePtrs.push_back(name.c_str());
        return true;
    }
};

FireRedVadOrt::FireRedVadOrt() : m_impl(new Impl()) {}

FireRedVadOrt::~FireRedVadOrt() {
    m_impl->release();
    delete m_impl;
}

bool FireRedVadOrt::init(const std::string& modelDir, float speechThreshold) {
    auto root = std::filesystem::path(modelDir);
    auto model = root / "fireredvad_stream_vad_with_cache.onnx";
    auto cmvn = root / "cmvn.ark";
    if (!fileExists(model) || !fileExists(cmvn)) {
        pcLogf(PcLogLevel::Warning, "VAD", "FireRedVAD payload missing under %s", root.string().c_str());
        return false;
    }

    m_impl->threshold = speechThreshold;
    m_impl->api = OrtGetApiBase()->GetApi(ORT_API_VERSION);
    if (!m_impl->api) return false;
    if (!m_impl->check(m_impl->api->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "pocketvoice-fireredvad", &m_impl->env), "create env")) return false;
    if (!m_impl->check(m_impl->api->CreateSessionOptions(&m_impl->sessionOptions), "create session options")) return false;
    m_impl->check(m_impl->api->SetIntraOpNumThreads(m_impl->sessionOptions, 1), "set intra threads");
    m_impl->check(m_impl->api->SetInterOpNumThreads(m_impl->sessionOptions, 1), "set inter threads");
    std::wstring modelPath = widenPath(model);
    if (!m_impl->check(m_impl->api->CreateSession(m_impl->env, modelPath.c_str(), m_impl->sessionOptions, &m_impl->session), "create session")) return false;
    if (!m_impl->check(m_impl->api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &m_impl->memoryInfo), "create memory info")) return false;
    if (!m_impl->collectNames()) return false;
    if (!loadKaldiCmvn(cmvn, m_impl->cmvnMeans, m_impl->cmvnIstd)) {
        pcLogf(PcLogLevel::Error, "VAD", "FireRedVAD failed to read CMVN: %s", cmvn.string().c_str());
        return false;
    }
    m_impl->cache.assign(static_cast<size_t>(kCacheSize * kCacheLen), 0.0f);
    m_impl->fbank = std::make_unique<vad::Fbank>(kFeatDim, kSampleRate, kFrameLengthSamples, kFrameShiftSamples);
    reset();
    pcLogf(PcLogLevel::Info, "VAD", "FireRedVAD ONNX initialized: %s", model.string().c_str());
    return true;
}

std::vector<FireRedVadFrame> FireRedVadOrt::process(const float* samples, size_t numSamples) {
    std::vector<FireRedVadFrame> frames;
    if (!m_impl->session || !samples || numSamples == 0) return frames;

    m_impl->pending.insert(m_impl->pending.end(), samples, samples + numSamples);
    while (m_impl->pending.size() >= kFrameShiftSamples) {
        std::vector<float> frame(m_impl->pending.begin(), m_impl->pending.begin() + kFrameShiftSamples);
        m_impl->pending.erase(m_impl->pending.begin(), m_impl->pending.begin() + kFrameShiftSamples);

        for (float sample : frame) {
            m_impl->audioWindow.push_back(static_cast<float>(toPcm16(sample)));
        }
        while (m_impl->audioWindow.size() > kFrameLengthSamples) {
            m_impl->audioWindow.pop_front();
        }
        if (m_impl->audioWindow.size() < kFrameLengthSamples) {
            frames.push_back({frame, 0.0f, false});
            continue;
        }

        std::vector<float> window(m_impl->audioWindow.begin(), m_impl->audioWindow.end());
        std::vector<float> features;
        int numFrames = m_impl->fbank->Compute(window, &features);
        if (numFrames <= 0 || features.size() < kFeatDim) {
            frames.push_back({frame, 0.0f, false});
            continue;
        }

        std::vector<float> feat(features.end() - kFeatDim, features.end());
        applyCmvn(m_impl->cmvnMeans, m_impl->cmvnIstd, feat);

        int64_t featShape[] = {1, kFeatDim};
        int64_t cacheShape[] = {1, kCacheSize, kCacheLen};
        OrtValue* inputFeat = nullptr;
        OrtValue* inputCache = nullptr;
        if (!m_impl->check(m_impl->api->CreateTensorWithDataAsOrtValue(
                m_impl->memoryInfo, feat.data(), feat.size() * sizeof(float), featShape, 2,
                ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &inputFeat), "create feat tensor")) {
            break;
        }
        if (!m_impl->check(m_impl->api->CreateTensorWithDataAsOrtValue(
                m_impl->memoryInfo, m_impl->cache.data(), m_impl->cache.size() * sizeof(float), cacheShape, 3,
                ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &inputCache), "create cache tensor")) {
            m_impl->api->ReleaseValue(inputFeat);
            break;
        }

        OrtValue* inputs[] = {inputFeat, inputCache};
        OrtValue* outputs[] = {nullptr, nullptr};
        bool ok = m_impl->check(m_impl->api->Run(
            m_impl->session, nullptr,
            m_impl->inputNamePtrs.data(), inputs, 2,
            m_impl->outputNamePtrs.data(), 2,
            outputs), "run");
        m_impl->api->ReleaseValue(inputFeat);
        m_impl->api->ReleaseValue(inputCache);
        if (!ok) break;

        float* probs = nullptr;
        float* nextCache = nullptr;
        m_impl->check(m_impl->api->GetTensorMutableData(outputs[0], reinterpret_cast<void**>(&probs)), "read probs");
        m_impl->check(m_impl->api->GetTensorMutableData(outputs[1], reinterpret_cast<void**>(&nextCache)), "read cache");
        float confidence = probs ? probs[0] : 0.0f;
        if (nextCache) {
            std::memcpy(m_impl->cache.data(), nextCache, m_impl->cache.size() * sizeof(float));
        }
        m_impl->api->ReleaseValue(outputs[0]);
        m_impl->api->ReleaseValue(outputs[1]);

        frames.push_back({frame, confidence, confidence >= m_impl->threshold});
    }
    return frames;
}

void FireRedVadOrt::reset() {
    if (!m_impl) return;
    m_impl->audioWindow.clear();
    m_impl->pending.clear();
    std::fill(m_impl->cache.begin(), m_impl->cache.end(), 0.0f);
    if (m_impl->fbank) m_impl->fbank->reset();
}

}  // namespace stt
