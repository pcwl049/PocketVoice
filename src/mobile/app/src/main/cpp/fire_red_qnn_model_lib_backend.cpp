#include "fire_red_qnn_model_lib_backend.h"

#ifndef STT_ENGINE_METADATA_ONLY
#include <android/log.h>
#endif

#include <cstring>
#include <cmath>
#include <cstdarg>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <limits>
#include <vector>
#include <dlfcn.h>
#include <memory>

// QNN headers (matching qwen3_qnn_backend.cpp)
#include "QnnInterface.h"
#include "QnnTypes.h"
#include "QnnBackend.h"
#include "QnnContext.h"
#include "QnnGraph.h"
#include "QnnTensor.h"
#include "System/QnnSystemInterface.h"
#include "QnnWrapperUtils.hpp"

#define LOG_TAG "FireRedQnnModelLib"
#ifndef LOGI
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#endif

namespace stt {

namespace {
constexpr int kStaticFrames = 1030;    // fixed time window
constexpr int kFeatDim = 80;           // fbank dim
constexpr int kOutFrames = 256;        // 1024/4 (4x downsampling)
constexpr int kVocab = 8667;           // CTC vocab

using Clock = std::chrono::steady_clock;

static void qnnLogCallback(const char* fmt, QnnLog_Level_t level,
                           uint64_t timestamp, va_list args) {
    char buf[1024];
    vsnprintf(buf, sizeof(buf), fmt, args);
    switch (level) {
        case QNN_LOG_LEVEL_ERROR:
            LOGE("[QNN] [%lu] %s", static_cast<unsigned long>(timestamp), buf);
            break;
        case QNN_LOG_LEVEL_WARN:
            LOGI("[QNN][W] [%lu] %s", static_cast<unsigned long>(timestamp), buf);
            break;
        default:
            LOGI("[QNN] [%lu] %s", static_cast<unsigned long>(timestamp), buf);
            break;
    }
}
}  // namespace

struct FireRedQnnModelLibBackend::Impl {
    bool initialized = false;

    // QNN handles
    void* qnnHtpLib = nullptr;
    void* qnnSystemLib = nullptr;
    void* modelLib = nullptr;
    const QnnInterface_t* qnnInterfaceHandle = nullptr;
    QNN_INTERFACE_VER_TYPE qnnInterface{};
    const QnnSystemInterface_t* qnnSystemInterfaceHandle = nullptr;
    QNN_SYSTEM_INTERFACE_VER_TYPE qnnSystemInterface{};
    Qnn_LogHandle_t logHandle = nullptr;
    Qnn_BackendHandle_t backendHandle = nullptr;
    Qnn_DeviceHandle_t deviceHandle = nullptr;
    Qnn_ContextHandle_t contextHandle = nullptr;
    Qnn_ProfileHandle_t profileHandle = nullptr;

    // composeGraphs
    typedef qnn_wrapper_api::ModelError_t (*ComposeGraphsFn_t)(
        Qnn_BackendHandle_t, QNN_INTERFACE_VER_TYPE, Qnn_ContextHandle_t,
        const qnn_wrapper_api::GraphConfigInfo_t**, uint32_t,
        qnn_wrapper_api::GraphInfo_t***, uint32_t*, bool,
        QnnLog_Callback_t, QnnLog_Level_t);
    typedef qnn_wrapper_api::ModelError_t (*FreeGraphsInfoFn_t)(
        qnn_wrapper_api::GraphInfo_t***, uint32_t);
    ComposeGraphsFn_t composeGraphsFn = nullptr;
    FreeGraphsInfoFn_t freeGraphsInfoFn = nullptr;
    qnn_wrapper_api::GraphInfo_t** graphsInfo = nullptr;
    uint32_t graphsCount = 0;

    // Rendered graph (first graph is our Engine)
    Qnn_GraphHandle_t graph = nullptr;
    std::string graphName;
    std::vector<Qnn_Tensor_t> inputTensors;
    std::vector<Qnn_Tensor_t> outputTensors;
    size_t xTensorIndex = 0;
    size_t logitsTensorIndex = 0;

    // Model metadata (CMVN)
    std::vector<float> cmvnMean;
    std::vector<float> cmvnInv;

    // Tokens
    std::vector<std::string> tokens;

    // Metrics
    int decodeMs = 0;
    std::string backendName = "fire_red_asr2_ctc_qnn(ml)";

    // ---- tensor helpers (mirror qwen3_qnn_backend.cpp) ----
    static const char* tensorName(const Qnn_Tensor_t& t) {
        if (t.version == QNN_TENSOR_VERSION_2) return t.v2.name;
        return t.v1.name;
    }
    static Qnn_DataType_t tensorDataType(const Qnn_Tensor_t& t) {
        if (t.version == QNN_TENSOR_VERSION_2) return t.v2.dataType;
        return t.v1.dataType;
    }
    static const Qnn_QuantizeParams_t& tensorQuantParams(const Qnn_Tensor_t& t) {
        if (t.version == QNN_TENSOR_VERSION_2) return t.v2.quantizeParams;
        return t.v1.quantizeParams;
    }
    static size_t tensorElementCount(const Qnn_Tensor_t& t) {
        size_t count = 1;
        if (t.version == QNN_TENSOR_VERSION_2) {
            for (uint32_t i = 0; i < t.v2.rank; ++i) count *= t.v2.dimensions[i];
        } else {
            for (uint32_t i = 0; i < t.v1.rank; ++i) count *= t.v1.dimensions[i];
        }
        return count;
    }
    static size_t tensorElementSize(const Qnn_Tensor_t& t) {
        switch (tensorDataType(t)) {
            case QNN_DATATYPE_FLOAT_16: return 2;
            case QNN_DATATYPE_FLOAT_32: return 4;
            case QNN_DATATYPE_UFIXED_POINT_8:
            case QNN_DATATYPE_SFIXED_POINT_8:
            case QNN_DATATYPE_UFIXED_POINT_16:
            case QNN_DATATYPE_SFIXED_POINT_16:
            case QNN_DATATYPE_UINT_16:
            case QNN_DATATYPE_INT_16: return 2;
            case QNN_DATATYPE_UINT_32:
            case QNN_DATATYPE_INT_32: return 4;
            case QNN_DATATYPE_INT_64:
            case QNN_DATATYPE_UINT_64: return 8;
            default: return 4;
        }
    }
    static float dequantize(const Qnn_Tensor_t& t, const void* raw, size_t idx) {
        const auto& qp = tensorQuantParams(t);
        if (qp.encodingDefinition == QNN_DEFINITION_DEFINED &&
            qp.quantizationEncoding == QNN_QUANTIZATION_ENCODING_SCALE_OFFSET) {
            const float scale = qp.scaleOffsetEncoding.scale;
            const int32_t offset = qp.scaleOffsetEncoding.offset;
            switch (tensorDataType(t)) {
                case QNN_DATATYPE_UFIXED_POINT_8: {
                    const auto* p = reinterpret_cast<const uint8_t*>(raw);
                    return (static_cast<float>(p[idx]) + offset) * scale;
                }
                case QNN_DATATYPE_UFIXED_POINT_16: {
                    const auto* p = reinterpret_cast<const uint16_t*>(raw);
                    return (static_cast<float>(p[idx]) + offset) * scale;
                }
                default: break;
            }
        }
        switch (tensorDataType(t)) {
            case QNN_DATATYPE_FLOAT_16: {
                const auto* p = reinterpret_cast<const uint16_t*>(raw);
                // minimal half-float -> float
                uint16_t h = p[idx];
                uint32_t sign = h & 0x8000u;
                uint32_t exp = (h >> 10) & 0x1fu;
                uint32_t mant = h & 0x3ffu;
                float f = 0.0f;
                if (exp == 0 && mant == 0) {
                    f = 0.0f;
                } else if (exp == 31) {
                    f = mant ? std::nanf("") : (sign ? -INFINITY : INFINITY);
                } else if (exp == 0) {
                    f = std::ldexp(static_cast<float>(mant), -24);
                } else {
                    f = std::ldexp(static_cast<float>(mant | 0x400u), static_cast<int>(exp) - 25);
                }
                return sign ? -f : f;
            }
            case QNN_DATATYPE_FLOAT_32: {
                const auto* p = reinterpret_cast<const float*>(raw);
                return p[idx];
            }
            default: return 0.0f;
        }
    }
    static uint16_t floatToHalf(float f) {
        uint32_t x;
        std::memcpy(&x, &f, 4);
        uint32_t sign = (x >> 16) & 0x8000u;
        int32_t exp = static_cast<int32_t>((x >> 23) & 0xffu) - 127 + 15;
        uint32_t mant = x & 0x7fffffu;
        if (exp <= 0) {
            if (exp < -10) return static_cast<uint16_t>(sign);
            mant = (mant | 0x800000u) >> (1 - exp);
            return static_cast<uint16_t>(sign | (mant >> 13));
        } else if (exp >= 31) {
            return static_cast<uint16_t>(sign | 0x7c00u | (mant ? 0x200u : 0));
        }
        return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exp) << 10) | (mant >> 13));
    }
    static void setClientBufRaw(Qnn_Tensor_t& t, void* data, uint32_t size) {
        if (t.version == QNN_TENSOR_VERSION_2) {
            t.v2.memType = QNN_TENSORMEMTYPE_RAW;
            t.v2.clientBuf.data = data;
            t.v2.clientBuf.dataSize = size;
        } else {
            t.v1.memType = QNN_TENSORMEMTYPE_RAW;
            t.v1.clientBuf.data = data;
            t.v1.clientBuf.dataSize = size;
        }
    }
    static void writeFloatInputToTensor(const float* src, size_t count,
                                        const Qnn_Tensor_t& tensor, void* dst) {
        if (!src || !dst) return;
        const size_t tensorCount = tensorElementCount(tensor);
        const size_t copyCount = std::min(count, tensorCount);
        switch (tensorDataType(tensor)) {
            case QNN_DATATYPE_FLOAT_32: {
                std::memset(dst, 0, tensorCount * sizeof(float));
                std::memcpy(dst, src, copyCount * sizeof(float));
                return;
            }
            case QNN_DATATYPE_FLOAT_16: {
                auto* out = reinterpret_cast<uint16_t*>(dst);
                std::fill(out, out + tensorCount, static_cast<uint16_t>(0));
                for (size_t i = 0; i < copyCount; ++i) out[i] = floatToHalf(src[i]);
                return;
            }
            case QNN_DATATYPE_UFIXED_POINT_8: {
                auto* out = reinterpret_cast<uint8_t*>(dst);
                const auto& params = tensorQuantParams(tensor);
                const float scale = params.encodingDefinition == QNN_DEFINITION_DEFINED &&
                            params.quantizationEncoding == QNN_QUANTIZATION_ENCODING_SCALE_OFFSET
                        ? params.scaleOffsetEncoding.scale
                        : 1.0f;
                const int32_t offset = params.encodingDefinition == QNN_DEFINITION_DEFINED &&
                            params.quantizationEncoding == QNN_QUANTIZATION_ENCODING_SCALE_OFFSET
                        ? params.scaleOffsetEncoding.offset
                        : 0;
                std::fill(out, out + tensorCount, static_cast<uint8_t>(0));
                for (size_t i = 0; i < copyCount; ++i) {
                    float q = src[i] / scale - offset;
                    out[i] = static_cast<uint8_t>(std::max(0.0f, std::min(255.0f, q)));
                }
                return;
            }
            case QNN_DATATYPE_UFIXED_POINT_16: {
                auto* out = reinterpret_cast<uint16_t*>(dst);
                const auto& params = tensorQuantParams(tensor);
                const float scale = params.encodingDefinition == QNN_DEFINITION_DEFINED &&
                            params.quantizationEncoding == QNN_QUANTIZATION_ENCODING_SCALE_OFFSET
                        ? params.scaleOffsetEncoding.scale
                        : 1.0f;
                const int32_t offset = params.encodingDefinition == QNN_DEFINITION_DEFINED &&
                            params.quantizationEncoding == QNN_QUANTIZATION_ENCODING_SCALE_OFFSET
                        ? params.scaleOffsetEncoding.offset
                        : 0;
                std::fill(out, out + tensorCount, static_cast<uint16_t>(0));
                for (size_t i = 0; i < copyCount; ++i) {
                    float q = src[i] / scale - offset;
                    out[i] = static_cast<uint16_t>(std::max(0.0f, std::min(65535.0f, q)));
                }
                return;
            }
            default: {
                std::memset(dst, 0, tensorCount * tensorElementSize(tensor));
                return;
            }
        }
    }
};

FireRedQnnModelLibBackend::FireRedQnnModelLibBackend() : m_impl(new Impl) {}
FireRedQnnModelLibBackend::~FireRedQnnModelLibBackend() { release(); delete m_impl; }

bool FireRedQnnModelLibBackend::init(const std::string& modelDir,
                                     const std::string& qnnLibDir) {
    Impl& im = *m_impl;
    if (im.initialized) return true;

    // ---- tokens ----
    {
        std::ifstream tf(modelDir + "/tokens.txt");
        if (!tf.is_open()) {
            LOGE("cannot open tokens: %s", (modelDir + "/tokens.txt").c_str());
            return false;
        }
        std::string line;
        while (std::getline(tf, line)) {
            if (line.empty()) continue;
            // tokens.txt format: "<symbol> <id>" per line; keep the symbol only.
            std::string sym = line;
            size_t sp = line.find_first_of(" \t");
            if (sp != std::string::npos) sym = line.substr(0, sp);
            im.tokens.push_back(sym);
        }
        LOGI("tokens loaded: %zu entries", im.tokens.size());
    }

    // ---- CMVN from model metadata file (same as firered_ctc backend) ----
    // The ORT backend reads cmvn from model.onnx metadata; the model-lib has no
    // metadata, so read from an optional cmvn.txt in the model dir.
    {
        std::ifstream cf(modelDir + "/cmvn.txt");
        if (cf.is_open()) {
            float v;
            bool mean = true;
            im.cmvnMean.clear();
            im.cmvnInv.clear();
            while (cf >> v) {
                if (mean) im.cmvnMean.push_back(v);
                else im.cmvnInv.push_back(v);
                if (im.cmvnMean.size() == kFeatDim) mean = false;
            }
            LOGI("cmvn loaded: mean=%zu inv=%zu", im.cmvnMean.size(), im.cmvnInv.size());
        } else {
            LOGI("no cmvn.txt found; using identity");
            im.cmvnInv.assign(kFeatDim, 1.0f);
            im.cmvnMean.assign(kFeatDim, 0.0f);
        }
    }

    // ---- Load QNN HTP backend ----
    std::string qnnHtpPath = qnnLibDir + "/libQnnHtp.so";
    im.qnnHtpLib = dlopen(qnnHtpPath.c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (!im.qnnHtpLib) {
        LOGE("Failed to load libQnnHtp.so: %s", dlerror());
        return false;
    }
    LOGI("Loaded libQnnHtp.so");

    typedef Qnn_ErrorHandle_t (*QnnInterface_getProvidersFn_t)(
        const QnnInterface_t*** providerList, uint32_t* numProviders);
    auto getProviders = (QnnInterface_getProvidersFn_t)dlsym(im.qnnHtpLib, "QnnInterface_getProviders");
    if (!getProviders) {
        LOGE("Failed to find QnnInterface_getProviders: %s", dlerror());
        release();
        return false;
    }
    const QnnInterface_t** providerList = nullptr;
    uint32_t numProviders = 0;
    Qnn_ErrorHandle_t err = getProviders(&providerList, &numProviders);
    if (err != QNN_SUCCESS || numProviders == 0 || !providerList) {
        LOGE("QnnInterface_getProviders failed: err=%lu", (unsigned long)err);
        release();
        return false;
    }
    im.qnnInterfaceHandle = providerList[0];
    im.qnnInterface = im.qnnInterfaceHandle->QNN_INTERFACE_VER_NAME;
    LOGI("QNN interface obtained: backend=%u", im.qnnInterfaceHandle->backendId);

    if (!im.qnnInterface.backendCreate || !im.qnnInterface.contextCreate ||
        !im.qnnInterface.graphExecute) {
        LOGE("critical QNN function pointers missing");
        release();
        return false;
    }

    // ---- Load QNN System library ----
    std::string qnnSystemPath = qnnLibDir + "/libQnnSystem.so";
    im.qnnSystemLib = dlopen(qnnSystemPath.c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (!im.qnnSystemLib) {
        LOGW("Failed to load libQnnSystem.so: %s (non-fatal)", dlerror());
    } else {
        typedef Qnn_ErrorHandle_t (*QnnSystemInterface_getProvidersFn_t)(
            const QnnSystemInterface_t*** providerList, uint32_t* numProviders);
        auto getSystemProviders = (QnnSystemInterface_getProvidersFn_t)dlsym(
            im.qnnSystemLib, "QnnSystemInterface_getProviders");
        if (getSystemProviders) {
            const QnnSystemInterface_t** sysProviderList = nullptr;
            uint32_t sysNumProviders = 0;
            err = getSystemProviders(&sysProviderList, &sysNumProviders);
            if (err == QNN_SUCCESS && sysNumProviders > 0 && sysProviderList) {
                im.qnnSystemInterfaceHandle = sysProviderList[0];
                im.qnnSystemInterface = im.qnnSystemInterfaceHandle->QNN_SYSTEM_INTERFACE_VER_NAME;
                LOGI("QNN System interface obtained");
            }
        }
    }

    // ---- QNN logging ----
    if (im.qnnInterface.logCreate) {
        err = im.qnnInterface.logCreate(qnnLogCallback, QNN_LOG_LEVEL_INFO, &im.logHandle);
        if (err != QNN_SUCCESS) LOGW("QnnLog_create failed: %lu (non-fatal)", (unsigned long)err);
    }

    // ---- Create backend ----
    err = im.qnnInterface.backendCreate(im.logHandle, nullptr, &im.backendHandle);
    if (err != QNN_SUCCESS) {
        LOGE("QnnBackend_create failed: %lu", (unsigned long)err);
        release();
        return false;
    }
    LOGI("QNN backend created");

    // ---- Create device (SM8735) ----
    if (im.qnnInterface.deviceCreate) {
        err = im.qnnInterface.deviceCreate(im.logHandle, nullptr, &im.deviceHandle);
        if (err != QNN_SUCCESS && err != QNN_DEVICE_ERROR_UNSUPPORTED_FEATURE) {
            LOGE("QnnDevice_create failed: %lu", (unsigned long)err);
            release();
            return false;
        }
        LOGI("QNN device created (err=%lu)", (unsigned long)err);
    }

    // ---- Create context ----
    err = im.qnnInterface.contextCreate(
        im.backendHandle, im.deviceHandle, nullptr, &im.contextHandle);
    if (err != QNN_SUCCESS) {
        LOGE("QnnContext_create failed: %lu", (unsigned long)err);
        release();
        return false;
    }
    LOGI("QNN context created");

    // ---- Load libmodel.so ----
    std::string modelLibPath = modelDir + "/libmodel.so";
    im.modelLib = dlopen(modelLibPath.c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (!im.modelLib) {
        LOGE("Failed to load %s: %s", modelLibPath.c_str(), dlerror());
        release();
        return false;
    }
    LOGI("Loaded %s", modelLibPath.c_str());

    im.composeGraphsFn = (Impl::ComposeGraphsFn_t)dlsym(im.modelLib, "QnnModel_composeGraphs");
    if (!im.composeGraphsFn) {
        LOGE("Failed to find QnnModel_composeGraphs: %s", dlerror());
        release();
        return false;
    }
    im.freeGraphsInfoFn = (Impl::FreeGraphsInfoFn_t)dlsym(im.modelLib, "QnnModel_freeGraphsInfo");
    LOGI("composeGraphs function found");

    // ---- Compose graphs ----
    const auto t0 = Clock::now();
    qnn_wrapper_api::ModelError_t modelErr = im.composeGraphsFn(
        im.backendHandle, im.qnnInterface, im.contextHandle,
        nullptr, 0, &im.graphsInfo, &im.graphsCount, false,
        qnnLogCallback, QNN_LOG_LEVEL_INFO);
    const auto composeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - t0).count();
    if (modelErr != qnn_wrapper_api::MODEL_NO_ERROR) {
        LOGE("composeGraphs failed: %d", modelErr);
        release();
        return false;
    }
    LOGI("composeGraphs succeeded: %u graph(s), %lld ms",
         im.graphsCount, (long long)composeMs);

    // ---- Pick first graph + finalize ----
    bool found = false;
    for (uint32_t g = 0; g < im.graphsCount; g++) {
        auto* gi = im.graphsInfo[g];
        if (!gi || !gi->graph) continue;
        Qnn_ErrorHandle_t ferr = im.qnnInterface.graphFinalize(gi->graph, nullptr, nullptr);
        if (ferr != QNN_SUCCESS) {
            LOGE("graphFinalize failed for '%s'", gi->graphName ? gi->graphName : "?");
            continue;
        }
        LOGI("graph '%s' finalized (%u in, %u out)",
             gi->graphName ? gi->graphName : "?",
             gi->numInputTensors, gi->numOutputTensors);
        im.graph = gi->graph;
        im.graphName = gi->graphName ? gi->graphName : "";
        im.inputTensors.assign(gi->inputTensors, gi->inputTensors + gi->numInputTensors);
        im.outputTensors.assign(gi->outputTensors, gi->outputTensors + gi->numOutputTensors);
        // Locate x input and log_probs output
        for (size_t i = 0; i < im.inputTensors.size(); i++) {
            const char* nm = Impl::tensorName(im.inputTensors[i]);
            LOGI("  input[%zu]: %s", i, nm ? nm : "?");
            if (nm && std::strcmp(nm, "x") == 0) im.xTensorIndex = i;
        }
        for (size_t i = 0; i < im.outputTensors.size(); i++) {
            const char* nm = Impl::tensorName(im.outputTensors[i]);
            LOGI("  output[%zu]: %s", i, nm ? nm : "?");
            if (nm && (std::strcmp(nm, "log_probs") == 0 ||
                       std::strcmp(nm, "log_probs_0") == 0)) {
                im.logitsTensorIndex = i;
            }
        }
        found = true;
        break;
    }
    if (!found) {
        LOGE("no finalized graph");
        release();
        return false;
    }

    im.initialized = true;
    im.backendName = "fire_red_asr2_ctc_qnn";  // real HTP, no _cpu
    LOGI("backend ready (%s)", im.backendName.c_str());
    return true;
}

bool FireRedQnnModelLibBackend::recognize(const float* fbank, int frames,
                                          std::string* text) {
    if (!m_impl || !m_impl->initialized || !m_impl->graph || !fbank || !text) return false;
    Impl& im = *m_impl;
    text->clear();

    try {
        const int realT = frames;
        const int padT = std::min(realT, static_cast<int>(kStaticFrames));

        // x tensor layout is {1, 1030, 80} = TIME-FIRST (NTF, as exported by the
        // QAIRT converter with --input_layout x NTF). Input fbank comes in as
        // [time=T][feature=80]; feed it directly (column-addressable, no
        // transpose needed). Audio longer than 1030 frames is truncated.
        std::vector<float> xBuf(static_cast<size_t>(kStaticFrames) * kFeatDim, 0.0f);
        std::memcpy(xBuf.data(), fbank, static_cast<size_t>(padT) * kFeatDim * sizeof(float));
        if (realT > kStaticFrames) {
            LOGE("audio %d frames exceeds static window %d, truncating tail",
                 realT, kStaticFrames);
        }

        // ---- bind input ----
        std::vector<std::vector<uint8_t>> inBuffers(im.inputTensors.size());
        std::vector<Qnn_Tensor_t> inputs = im.inputTensors;  // copy
        for (size_t i = 0; i < inputs.size(); i++) {
            const size_t bytes = Impl::tensorElementCount(inputs[i]) *
                                 Impl::tensorElementSize(inputs[i]);
            inBuffers[i].resize(bytes, 0);
            if (i == im.xTensorIndex) {
                Impl::writeFloatInputToTensor(xBuf.data(), xBuf.size(),
                                              inputs[i], inBuffers[i].data());
            }
            Impl::setClientBufRaw(inputs[i], inBuffers[i].data(),
                                  static_cast<uint32_t>(bytes));
        }

        // ---- bind output ----
        std::vector<std::vector<uint8_t>> outBuffers(im.outputTensors.size());
        std::vector<Qnn_Tensor_t> outputs = im.outputTensors;  // copy
        for (size_t i = 0; i < outputs.size(); i++) {
            const size_t bytes = Impl::tensorElementCount(outputs[i]) *
                                 Impl::tensorElementSize(outputs[i]);
            outBuffers[i].resize(bytes, 0);
            Impl::setClientBufRaw(outputs[i], outBuffers[i].data(),
                                  static_cast<uint32_t>(bytes));
        }

        // ---- execute ----
        const auto t0 = Clock::now();
        Qnn_ErrorHandle_t err = im.qnnInterface.graphExecute(
            im.graph,
            inputs.data(), static_cast<uint32_t>(inputs.size()),
            outputs.data(), static_cast<uint32_t>(outputs.size()),
            im.profileHandle, nullptr);
        const auto t1 = Clock::now();
        im.decodeMs = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
            t1 - t0).count());
        if (err != QNN_SUCCESS) {
            LOGE("graphExecute failed: %lu", (unsigned long)err);
            return false;
        }

        // ---- read log_probs + CTC greedy decode ----
        const auto& lpTensor = outputs[im.logitsTensorIndex];
        const size_t outCount = Impl::tensorElementCount(lpTensor);
        const size_t outFrames = outCount / kVocab;  // 256
        std::vector<float> logp(outCount);
        for (size_t t = 0; t < outCount; t++) {
            logp[t] = Impl::dequantize(lpTensor, outBuffers[im.logitsTensorIndex].data(), t);
        }

        const int64_t numValid = std::min<int64_t>(outFrames,
                                                   (static_cast<int64_t>(realT) + 3) / 4);
        int prevId = -1;
        std::vector<int> ids;
        ids.reserve(static_cast<size_t>(numValid));
        for (int64_t t = 0; t < numValid; t++) {
            const float* row = logp.data() + t * kVocab;
            int best = 0;
            float bestV = row[0];
            for (int64_t k = 1; k < kVocab; k++) {
                if (row[k] > bestV) { bestV = row[k]; best = static_cast<int>(k); }
            }
            if (best != 0 && best != prevId) ids.push_back(best);
            prevId = best;
        }

        for (int id : ids) {
            if (id < 0 || id >= static_cast<int>(im.tokens.size())) continue;
            std::string sym = im.tokens[id];
            if (sym.empty() || sym == "<blank>") continue;
            // Strip SentencePiece word-boundary markers (U+2581 '▁')
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
    } catch (const std::exception& e) {
        LOGE("recognize failed: %s", e.what());
        return false;
    }
}

bool FireRedQnnModelLibBackend::isInitialized() const {
    return m_impl && m_impl->initialized && m_impl->graph;
}
const std::string& FireRedQnnModelLibBackend::backendName() const { return m_impl->backendName; }
int FireRedQnnModelLibBackend::lastDecodeMs() const { return m_impl->decodeMs; }
bool FireRedQnnModelLibBackend::qnnActive() const { return m_impl->initialized; }

bool FireRedQnnModelLibBackend::cmvn(const float** mean, const float** invStd,
                                     int* dim) const {
    if (!m_impl || m_impl->cmvnMean.empty()) return false;
    *mean = m_impl->cmvnMean.data();
    *invStd = m_impl->cmvnInv.data();
    *dim = static_cast<int>(m_impl->cmvnMean.size());
    return true;
}

void FireRedQnnModelLibBackend::release() {
    if (!m_impl) return;
    Impl& im = *m_impl;
    if (im.freeGraphsInfoFn && im.graphsInfo) {
        im.freeGraphsInfoFn(&im.graphsInfo, im.graphsCount);
        im.graphsInfo = nullptr;
        im.graphsCount = 0;
    }
    if (im.modelLib) { dlclose(im.modelLib); im.modelLib = nullptr; }
    if (im.contextHandle && im.qnnInterface.contextFree) {
        im.qnnInterface.contextFree(im.contextHandle, nullptr);
        im.contextHandle = nullptr;
    }
    if (im.deviceHandle && im.qnnInterface.deviceFree) {
        im.qnnInterface.deviceFree(im.deviceHandle);
        im.deviceHandle = nullptr;
    }
    if (im.backendHandle && im.qnnInterface.backendFree) {
        im.qnnInterface.backendFree(im.backendHandle);
        im.backendHandle = nullptr;
    }
    if (im.logHandle && im.qnnInterface.logFree) {
        im.qnnInterface.logFree(im.logHandle);
        im.logHandle = nullptr;
    }
    if (im.qnnSystemLib) { dlclose(im.qnnSystemLib); im.qnnSystemLib = nullptr; }
    if (im.qnnHtpLib) { dlclose(im.qnnHtpLib); im.qnnHtpLib = nullptr; }
    im.graph = nullptr;
    im.inputTensors.clear();
    im.outputTensors.clear();
    im.initialized = false;
}

}  // namespace stt