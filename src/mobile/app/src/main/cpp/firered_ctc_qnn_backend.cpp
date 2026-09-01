// stt/mobile/app/src/main/cpp/firered_ctc_qnn_backend.cpp
//
// FireRedASR2-CTC via ONNX Runtime QNN Execution Provider (HTP).
//
// The model is a static QDQ uint8 quantization of the original
// sherpa-onnx FireRedASR2-CTC int8 ONNX (encoder + CTC head only).
//   inputs : x [1, T, 80] float32, x_lens [1] int64
//   outputs: log_probs [1, T/4, 8667] float32, log_probs_len [1] int64
//   CTC blank id = 0 (<blank>)

#include "firered_ctc_qnn_backend.h"

#include <android/log.h>
#include <chrono>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <onnxruntime_cxx_api.h>

#include <nlohmann/json.hpp>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "FireRedQnn", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "FireRedQnn", __VA_ARGS__)

namespace stt {

namespace {

struct QnnEpTuning {
    std::string perfMode;        // htp_performance_mode ("" = default burst)
    int vtcmMb = 0;              // 0 = unset
    int rpcControlLatency = 0;   // us, 0 = unset
    int rpcPollingTime = 0;      // us, 0 = unset
};

static QnnEpTuning loadEpTuning(const std::string& modelPath) {
    QnnEpTuning tun;
    std::string dir = modelPath;
    size_t slash = dir.find_last_of("/\\");
    if (slash != std::string::npos) dir = dir.substr(0, slash);
    std::string cfg = dir + "/qnn_ep_options.json";
    std::ifstream f(cfg);
    if (!f.is_open()) return tun;
    try {
        nlohmann::json j = nlohmann::json::parse(f);
        if (j.contains("htp_performance_mode")) tun.perfMode = j["htp_performance_mode"].get<std::string>();
        if (j.contains("vtcm_mb")) tun.vtcmMb = j["vtcm_mb"].get<int>();
        if (j.contains("rpc_control_latency")) tun.rpcControlLatency = j["rpc_control_latency"].get<int>();
        if (j.contains("rpc_polling_time")) tun.rpcPollingTime = j["rpc_polling_time"].get<int>();
        __android_log_print(ANDROID_LOG_INFO, "FireRedQnn",
            "ep tuning: mode=%s vtcm=%d rpcLat=%d rpcPoll=%d",
            tun.perfMode.c_str(), tun.vtcmMb, tun.rpcControlLatency, tun.rpcPollingTime);
    } catch (const std::exception& e) {
        __android_log_print(ANDROID_LOG_WARN, "FireRedQnn", "ep tuning parse failed: %s", e.what());
    }
    return tun;
}

struct TokenTable {
    std::vector<std::string> id2sym;
    bool load(const std::string& path) {
        std::ifstream is(path);
        if (!is.is_open()) {
            LOGE("cannot open tokens: %s", path.c_str());
            return false;
        }
        // "<token> <id>" per line; token may not contain spaces.
        std::string line;
        int maxId = -1;
        std::vector<std::pair<int, std::string>> entries;
        while (std::getline(is, line)) {
            // Trim trailing \r
            if (!line.empty() && line.back() == '\r') line.pop_back();
            std::istringstream iss(line);
            std::string sym;
            int id = -1;
            if (!(iss >> sym)) continue;
            if (iss.eof()) {
                id = std::atoi(sym.c_str());
                sym = " ";
            } else {
                iss >> id;
            }
            maxId = std::max(maxId, id);
            entries.emplace_back(id, sym);
        }
        id2sym.assign(static_cast<size_t>(maxId) + 1, "<err>");
        for (auto& e : entries) id2sym[static_cast<size_t>(e.first)] = e.second;
        return !id2sym.empty();
    }

    const std::string& at(int id) const {
        if (id < 0 || static_cast<size_t>(id) >= id2sym.size()) {
            static const std::string kErr = "<oob>";
            return kErr;
        }
        return id2sym[static_cast<size_t>(id)];
    }
};

}  // namespace

// Shared between the ORT logging sink and init(): ORT's QNN EP silently falls
// back to CPU when device/backend setup fails (GetCapability returns an empty
// result instead of throwing), so we must watch the log for the failure marker.
struct FireRedQnnEpStatus {
    bool setupFailed = false;
};

// ORT logging sink: forwards warnings/errors to logcat and flags silent QNN
// EP failures. GetAvailableProviders() only lists what the build was compiled
// with, so it cannot tell whether HTP actually initialized.
static void FireRedOrtLogSink(void* param, OrtLoggingLevel severity, const char* /*category*/,
                              const char* /*logid*/, const char* /*code_location*/, const char* message) {
    if (message == nullptr) return;
    if (severity == ORT_LOGGING_LEVEL_ERROR || severity == ORT_LOGGING_LEVEL_WARNING) {
        LOGI("ORT: %s", message);
        if (param != nullptr && strstr(message, "QNN SetupBackend failed") != nullptr) {
            static_cast<FireRedQnnEpStatus*>(param)->setupFailed = true;
        }
    }
}

struct FireRedCtcQnnBackend::Impl {
    Ort::Env env{nullptr};
    Ort::SessionOptions opts{nullptr};
    Ort::Session* session = nullptr;
    bool qnn = false;
    FireRedQnnEpStatus epStatus;
    TokenTable tokens;
    std::vector<float> cmvnMean;
    std::vector<float> cmvnInv;
    int decodeMs = 0;
    std::vector<int64_t> inputShape;  // [1, T, 80]
    std::vector<int64_t> lenShape;    // [1]

    std::string inX = "x";
    std::string inLen = "x_lens";
    std::string outLogits = "log_probs";
    std::string outLen = "log_probs_len";
};

FireRedCtcQnnBackend::FireRedCtcQnnBackend() : m_impl(new Impl) {}

FireRedCtcQnnBackend::~FireRedCtcQnnBackend() { release(); }

static void trimTrailingEOF(std::string& s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) s.pop_back();
}

bool FireRedCtcQnnBackend::init(const std::string& modelPath,
                                const std::string& tokensPath,
                                const std::string& qnnLibDir,
                                bool useHtp) {
    release();
    Impl& im = *m_impl;

    if (!im.tokens.load(tokensPath)) {
        LOGE("token table load failed");
        return false;
    }
    LOGI("tokens loaded: %zu entries", im.tokens.id2sym.size());

    im.env = Ort::Env{ORT_LOGGING_LEVEL_WARNING, "fire_red_ctc_qnn", &FireRedOrtLogSink, &im.epStatus};
    im.opts = Ort::SessionOptions{};
    im.opts.SetIntraOpNumThreads(2);
    im.opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

    // Optional session config used by qwen3/paraformer pipelines; keep defaults.

    if (useHtp) {
        // Try QNN HTP first; if unavailable, path is dropped automatically and
        // session creation proceeds with the remaining (CPU) providers.
        std::string backendPath = qnnLibDir.empty()
            ? "libQnnHtp.so"
            : (qnnLibDir + "/libQnnHtp.so");
        std::unordered_map<std::string, std::string> qnnOpts;
        qnnOpts["backend_path"] = backendPath;
        qnnOpts["device_id"] = "0";
        const QnnEpTuning tun = loadEpTuning(modelPath);
        qnnOpts["htp_performance_mode"] =
            tun.perfMode.empty() ? std::string("burst") : tun.perfMode;
        if (tun.vtcmMb > 0) qnnOpts["vtcm_mb"] = std::to_string(tun.vtcmMb);
        if (tun.rpcControlLatency > 0) qnnOpts["rpc_control_latency"] = std::to_string(tun.rpcControlLatency);
        if (tun.rpcPollingTime > 0) qnnOpts["rpc_polling_time"] = std::to_string(tun.rpcPollingTime);
        im.opts.AppendExecutionProvider("QNN", qnnOpts);
        LOGI("QNN EP appended backend_path=%s mode=%s", backendPath.c_str(),
             qnnOpts["htp_performance_mode"].c_str());
    } else {
        LOGI("CPU-only mode (no QNN EP)");
    }

    bool qnnSessionOk = false;
    try {
        im.session = new Ort::Session(im.env, modelPath.c_str(), im.opts);
        qnnSessionOk = true;
    } catch (const Ort::Exception& e) {
        if (useHtp) {
            // Retry without QNN — ephemeral device state must not block ASR.
            LOGE("QNN session failed (%s); retrying CPU-only", e.what());
            im.opts = Ort::SessionOptions{};
            im.opts.SetIntraOpNumThreads(2);
            im.opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
            try {
                im.session = new Ort::Session(im.env, modelPath.c_str(), im.opts);
            } catch (const Ort::Exception& e2) {
                LOGE("CPU session also failed: %s", e2.what());
                im.session = nullptr;
                return false;
            }
        } else {
            LOGE("session creation failed: %s", e.what());
            return false;
        }
    }

    // HTP is only "in use" when the QNN session was created without a fallback
    // AND the ORT log sink saw no "QNN SetupBackend failed" marker (the QNN EP
    // silently falls back to CPU in that case). GetAvailableProviders() only
    // reflects what the ORT build was compiled with and must not be used here.
    im.qnn = useHtp && qnnSessionOk && !im.epStatus.setupFailed;
    if (useHtp && !im.qnn) {
        LOGE("HTP unavailable (qnnSessionOk=%d qnnSetupFailed=%d); running on CPU",
             static_cast<int>(qnnSessionOk), static_cast<int>(im.epStatus.setupFailed));
    }
    m_initialized = true;
    // Diagnostic only: which EPs this ORT build was compiled with.
    try {
        auto ps = Ort::GetAvailableProviders();
        for (auto& p : ps) {
            LOGI("available provider: %s", p.c_str());
        }
    } catch (...) {
    }

    // Read fbank CMVN metadata (cmvn_mean / cmvn_inv_stddev, comma separated).
    try {
        Ort::AllocatorWithDefaultOptions allocator;
        Ort::ModelMetadata meta = im.session->GetModelMetadata();
        auto meanStr = meta.LookupCustomMetadataMapAllocated("cmvn_mean", allocator);
        auto invStr = meta.LookupCustomMetadataMapAllocated("cmvn_inv_stddev", allocator);
        if (meanStr && invStr) {
            auto parse = [](const char* s) {
                std::vector<float> v;
                std::stringstream ss(s);
                std::string tok;
                while (std::getline(ss, tok, ',')) {
                    v.push_back(std::strtof(tok.c_str(), nullptr));
                }
                return v;
            };
            im.cmvnMean = parse(meanStr.get());
            im.cmvnInv = parse(invStr.get());
            if (im.cmvnMean.empty() || im.cmvnMean.size() != im.cmvnInv.size()) {
                LOGI("cmvn metadata malformed (%zu / %zu dims) - ignored",
                     im.cmvnMean.size(), im.cmvnInv.size());
                im.cmvnMean.clear();
                im.cmvnInv.clear();
            } else {
                LOGI("cmvn loaded: %zu dims", im.cmvnMean.size());
            }
        } else {
            LOGI("model has no cmvn metadata");
        }
    } catch (const Ort::Exception& e) {
        LOGE("cmvn metadata read failed: %s", e.what());
    }

    // Read input/output names + shapes to be robust to ONNX naming.
    try {
        Ort::AllocatorWithDefaultOptions allocator;
        auto inputInfo = im.session->GetInputNameAllocated(0, allocator);
        im.inX = inputInfo.get();
        if (im.session->GetInputCount() > 1) {
            auto lenInfo = im.session->GetInputNameAllocated(1, allocator);
            im.inLen = lenInfo.get();
        }
        auto outInfo = im.session->GetOutputNameAllocated(0, allocator);
        im.outLogits = outInfo.get();
        if (im.session->GetOutputCount() > 1) {
            auto lenOut = im.session->GetOutputNameAllocated(1, allocator);
            im.outLen = lenOut.get();
        }
        LOGI("inputs: %s %s ; outputs: %s %s", im.inX.c_str(), im.inLen.c_str(),
             im.outLogits.c_str(), im.outLen.c_str());
    } catch (const Ort::Exception& e) {
        LOGE("io name query failed: %s", e.what());
    }

    m_backendName = im.qnn ? "fire_red_asr2_ctc_qnn" : "fire_red_asr2_ctc_qnn_cpu";
    LOGI("backend ready (%s)", m_backendName.c_str());
    return true;
}

bool FireRedCtcQnnBackend::recognize(const float* fbank, int frames,
                                     std::string* text) {
    if (!m_initialized || !m_impl->session || !fbank || frames <= 0 || !text) {
        return false;
    }
    Impl& im = *m_impl;
    text->clear();

    try {
        Ort::MemoryInfo memInfo = Ort::MemoryInfo::CreateCpu(
            OrtArenaAllocator, OrtMemTypeDefault);

        // Bucket the time axis to multiples of 32 frames: the QNN HTP graph
        // is prepared per input shape, so a handful of buckets keeps the
        // per-utterance graph warm-up cost at zero after the first call per
        // bucket (verified lossless: padded run matches exact-T argmax 1.0).
        const int64_t realT = frames;
        int64_t T = (realT + 31) & ~int64_t(31);
        std::vector<float> xBuf;
        const float* feedData = fbank;
        if (T != realT) {
            xBuf.assign(static_cast<size_t>(T) * 80, 0.0f);
            std::memcpy(xBuf.data(), fbank,
                        static_cast<size_t>(realT) * 80 * sizeof(float));
            feedData = xBuf.data();
        }

        // x [1, T, 80]
        std::vector<int64_t> xShape{1, T, 80};
        Ort::Value xVal = Ort::Value::CreateTensor<float>(
            memInfo, const_cast<float*>(feedData), static_cast<size_t>(T) * 80,
            xShape.data(), xShape.size());

        // x_lens [1] int64 (real frame count)
        std::vector<int64_t> lens{realT};
        std::array<int64_t, 1> lenShape{1};
        Ort::Value lenScalar = Ort::Value::CreateTensor<int64_t>(
            memInfo, lens.data(), 1, lenShape.data(), lenShape.size());

        std::vector<const char*> inputNames{im.inX.c_str(), im.inLen.c_str()};
        std::vector<Ort::Value> inVals;
        inVals.push_back(std::move(xVal));
        inVals.push_back(std::move(lenScalar));

        std::vector<const char*> outNames{im.outLogits.c_str(), im.outLen.c_str()};

        auto t0 = std::chrono::steady_clock::now();
        auto outs = im.session->Run(Ort::RunOptions{nullptr}, inputNames.data(),
                                    inVals.data(), inVals.size(), outNames.data(),
                                    outNames.size());
        auto t1 = std::chrono::steady_clock::now();
        im.decodeMs = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());

        if (outs.size() < 2) {
            LOGE("unexpected output count: %zu", outs.size());
            return false;
        }

        const float* logp = outs[0].GetTensorData<float>();
        auto shape = outs[0].GetTensorTypeAndShapeInfo().GetShape();
        if (shape.size() != 3) {
            LOGE("unexpected logits rank %zu", shape.size());
            return false;
        }
        const int64_t numFrames = shape[1];
        const int64_t vocab = shape[2];

        const int64_t* lenOut = outs[1].GetTensorData<int64_t>();
        int64_t numValid = lenOut ? lenOut[0] : numFrames;
        if (numValid > numFrames) numValid = numFrames;

        // CTC greedy decode: blank id 0, collapse repeats.
        int prevId = -1;
        std::vector<int> ids;
        ids.reserve(static_cast<size_t>(numValid));
        for (int64_t t = 0; t < numValid; ++t) {
            const float* row = logp + t * vocab;
            int best = 0;
            float bestV = row[0];
            for (int64_t k = 1; k < vocab; ++k) {
                if (row[k] > bestV) {
                    bestV = row[k];
                    best = static_cast<int>(k);
                }
            }
            if (best != 0 && best != prevId) ids.push_back(best);
            prevId = best;
        }

        for (int id : ids) {
            std::string sym = im.tokens.at(id);
            if (sym.empty() || sym == "<blank>") continue;
            // Strip SentencePiece word-boundary markers (U+2581 '▁',
            // UTF-8 e2 96 81); VRChat ChatBox text has no use for them.
            std::string cleaned;
            for (size_t i = 0; i < sym.size();) {
                if ((unsigned char)sym[i] == 0xe2 && i + 2 < sym.size() &&
                    (unsigned char)sym[i + 1] == 0x96 &&
                    (unsigned char)sym[i + 2] == 0x81) {
                    i += 3;
                    continue;
                }
                cleaned += sym[i++];
            }
            if (cleaned.empty()) continue;
            text->append(cleaned);
        }
        return true;
    } catch (const Ort::Exception& e) {
        LOGE("recognize failed: %s", e.what());
        return false;
    }
}

bool FireRedCtcQnnBackend::isInitialized() const { return m_initialized && m_impl->session; }

const std::string& FireRedCtcQnnBackend::backendName() const { return m_backendName; }

int FireRedCtcQnnBackend::lastDecodeMs() const { return m_impl->decodeMs; }

bool FireRedCtcQnnBackend::qnnActive() const { return m_impl->qnn; }

bool FireRedCtcQnnBackend::cmvn(const float** mean, const float** invStd, int* dim) const {
    if (m_impl->cmvnMean.empty()) return false;
    *mean = m_impl->cmvnMean.data();
    *invStd = m_impl->cmvnInv.data();
    *dim = static_cast<int>(m_impl->cmvnMean.size());
    return true;
}

void FireRedCtcQnnBackend::release() {
    if (m_impl) {
        delete m_impl->session;
        m_impl->session = nullptr;
        m_impl->env = Ort::Env{nullptr};
        m_impl->opts = Ort::SessionOptions{nullptr};
        m_impl->qnn = false;
        m_impl->decodeMs = 0;
        m_impl->epStatus.setupFailed = false;
    }
    m_initialized = false;
}

}  // namespace stt