#include "qwen3_qnn_backend.h"

#ifndef STT_ENGINE_METADATA_ONLY
#include <android/log.h>
#endif

#include <cstring>
#include <cmath>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <limits>
#include <vector>
#include <dlfcn.h>
#include <memory>

#include "onnxruntime_cxx_api.h"

// QNN headers - real C API from QAIRT SDK
#include "QnnInterface.h"
#include "QnnTypes.h"
#include "QnnBackend.h"
#include "QnnContext.h"
#include "QnnGraph.h"
#include "QnnTensor.h"
#include "QnnDevice.h"
#include "QnnLog.h"
#include "System/QnnSystemInterface.h"
#include "QnnWrapperUtils.hpp"

#ifndef STT_ENGINE_METADATA_ONLY
#define LOG_TAG "Qwen3Qnn"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#else
#define LOGI(...)
#define LOGE(...)
#define LOGW(...)
#endif

namespace stt {

// Fixed window size: must match the decoder artifact (decoder-w128 or decoder-w4)
// Currently using W=128 for full prompt context preservation
static constexpr int W = 128;
static constexpr int NUM_LAYERS = 28;
static constexpr int NUM_HEADS = 8;
static constexpr int HEAD_DIM = 128;
static constexpr int MAX_DECODE_STEPS = 512;
static constexpr int VOCAB_SIZE = 151936;
static constexpr int CONV_HIDDEN_DIM = 896;
static constexpr int ENCODER_HIDDEN_DIM = 1024;
static constexpr int ROPE_FREQ_DIM = 64;

static std::array<float, ROPE_FREQ_DIM> buildRopeInvFreq() {
    // Qwen3 uses theta=1000000 (not 10000) for RoPE
    // Formula: inv_freq = 1.0 / (theta ** (2*i / dim))
    // where theta=1000000, dim=128, i ranges from 0 to 63
    constexpr float kTheta = 1000000.0f;
    constexpr int kDim = 128;  // Full dimension, not half
    std::array<float, ROPE_FREQ_DIM> invFreq{};
    for (int i = 0; i < ROPE_FREQ_DIM; ++i) {
        invFreq[i] = 1.0f / std::pow(kTheta, static_cast<float>(2 * i) / kDim);
    }
    return invFreq;
}

static const std::array<float, ROPE_FREQ_DIM> kRopeInvFreq = buildRopeInvFreq();

// composeGraphs function type from generated libmodel.so
typedef qnn_wrapper_api::ModelError_t (*ComposeGraphsFnHandleType_t)(
    Qnn_BackendHandle_t,
    QNN_INTERFACE_VER_TYPE,
    Qnn_ContextHandle_t,
    const qnn_wrapper_api::GraphConfigInfo_t**,
    const uint32_t,
    qnn_wrapper_api::GraphInfo_t***,
    uint32_t*,
    bool,
    QnnLog_Callback_t,
    QnnLog_Level_t);

typedef qnn_wrapper_api::ModelError_t (*FreeGraphInfoFnHandleType_t)(
    qnn_wrapper_api::GraphInfo_t***, uint32_t);

// QNN log callback for Android
static void qnnLogCallback(const char* fmt, QnnLog_Level_t level, uint64_t timestamp, va_list args) {
    char buf[1024];
    vsnprintf(buf, sizeof(buf), fmt, args);
    switch (level) {
        case QNN_LOG_LEVEL_ERROR:
            __android_log_print(ANDROID_LOG_ERROR, "QNN", "[%lu] %s", static_cast<unsigned long>(timestamp), buf);
            break;
        case QNN_LOG_LEVEL_WARN:
            __android_log_print(ANDROID_LOG_WARN, "QNN", "[%lu] %s", static_cast<unsigned long>(timestamp), buf);
            break;
        case QNN_LOG_LEVEL_INFO:
            __android_log_print(ANDROID_LOG_INFO, "QNN", "[%lu] %s", static_cast<unsigned long>(timestamp), buf);
            break;
        case QNN_LOG_LEVEL_VERBOSE:
        case QNN_LOG_LEVEL_DEBUG:
        default:
            __android_log_print(ANDROID_LOG_DEBUG, "QNN", "[%lu] %s", static_cast<unsigned long>(timestamp), buf);
            break;
    }
}

struct Qwen3QnnBackend::Impl {
    // Library handles
    void* qnnHtpLib = nullptr;
    void* qnnSystemLib = nullptr;
    void* decoderLib = nullptr;

    // QNN interface (function pointers)
    const QnnInterface_t* qnnInterfaceHandle = nullptr;
    QNN_INTERFACE_VER_TYPE qnnInterface;  // the actual function pointer table

    // QNN System interface
    const QnnSystemInterface_t* qnnSystemInterfaceHandle = nullptr;
    QNN_SYSTEM_INTERFACE_VER_TYPE qnnSystemInterface;

    // composeGraphs from model.so
    ComposeGraphsFnHandleType_t composeGraphsFn = nullptr;
    FreeGraphInfoFnHandleType_t freeGraphInfoFn = nullptr;

    // QNN handles
    Qnn_LogHandle_t logHandle = nullptr;
    Qnn_BackendHandle_t backendHandle = nullptr;
    Qnn_DeviceHandle_t deviceHandle = nullptr;
    Qnn_ContextHandle_t contextHandle = nullptr;
    Qnn_GraphHandle_t decoderGraph = nullptr;

    // Graph info from composeGraphs
    qnn_wrapper_api::GraphInfo_t** graphsInfo = nullptr;
    uint32_t graphsCount = 0;

    // Full KV cache buffers [layers][max_steps][heads][head_dim]
    std::vector<std::vector<std::vector<std::vector<float>>>> fullKeyCache;
    std::vector<std::vector<std::vector<std::vector<float>>>> fullValueCache;

    // Current decode state
    int currentStep = 0;
    int lastArgmax = 0;
    float lastLogitsSum = 0.0f;

    // Per-step diagnostic records (Gate 3)
    std::vector<Qwen3QnnBackend::StepDiag> diagRecords;

    // Model paths
    std::string convFrontendPath;
    std::string encoderPath;
    std::string tokenizerPath;

    std::unique_ptr<Ort::Env> ortEnv;
    std::unique_ptr<Ort::SessionOptions> ortSessionOptions;
    Ort::MemoryInfo cpuMemoryInfo = Ort::MemoryInfo::CreateCpu(OrtDeviceAllocator, OrtMemTypeDefault);
    std::unique_ptr<Ort::Session> convSession;
    std::unique_ptr<Ort::Session> encoderSession;
    std::vector<std::string> convInputNames;
    std::vector<std::string> convOutputNames;
    std::vector<const char*> convInputNamePtrs;
    std::vector<const char*> convOutputNamePtrs;
    std::vector<std::string> encoderInputNames;
    std::vector<std::string> encoderOutputNames;
    std::vector<const char*> encoderInputNamePtrs;
    std::vector<const char*> encoderOutputNamePtrs;

    void resetCache() {
        currentStep = 0;
        diagRecords.clear();
        for (auto& layer : fullKeyCache)
            for (auto& step : layer)
                for (auto& head : step)
                    std::fill(head.begin(), head.end(), 0.0f);
        for (auto& layer : fullValueCache)
            for (auto& step : layer)
                for (auto& head : step)
                    std::fill(head.begin(), head.end(), 0.0f);
    }

    void allocateCache() {
        fullKeyCache.resize(NUM_LAYERS);
        fullValueCache.resize(NUM_LAYERS);
        for (int i = 0; i < NUM_LAYERS; i++) {
            fullKeyCache[i].resize(MAX_DECODE_STEPS);
            fullValueCache[i].resize(MAX_DECODE_STEPS);
            for (int j = 0; j < MAX_DECODE_STEPS; j++) {
                fullKeyCache[i][j].resize(NUM_HEADS, std::vector<float>(HEAD_DIM, 0.0f));
                fullValueCache[i][j].resize(NUM_HEADS, std::vector<float>(HEAD_DIM, 0.0f));
            }
        }
    }

    void prepareWindow(float* window, int layer, bool isKey) {
        // Prepare W=4 window from full cache
        // Shape: [1, W, 8, 128] = W * NUM_HEADS * HEAD_DIM floats
        auto& cache = isKey ? fullKeyCache[layer] : fullValueCache[layer];

        // Zero-fill first
        std::memset(window, 0, W * NUM_HEADS * HEAD_DIM * sizeof(float));

        // Take the last W valid positions, left-aligned
        int startStep = std::max(0, currentStep - W);
        int windowCount = std::min(W, currentStep);

        for (int w = 0; w < windowCount; w++) {
            int srcStep = startStep + w;
            if (srcStep < currentStep && srcStep < MAX_DECODE_STEPS) {
                int dstIdx = w * NUM_HEADS * HEAD_DIM;
                for (int h = 0; h < NUM_HEADS; h++) {
                    for (int d = 0; d < HEAD_DIM; d++) {
                        window[dstIdx + h * HEAD_DIM + d] = cache[srcStep][h][d];
                    }
                }
            }
        }
    }

    void updateCache(int layer, const float* keyDelta, const float* valueDelta) {
        if (currentStep >= MAX_DECODE_STEPS) return;
        for (int h = 0; h < NUM_HEADS; h++) {
            for (int d = 0; d < HEAD_DIM; d++) {
                int idx = h * HEAD_DIM + d;
                fullKeyCache[layer][currentStep][h][d] = keyDelta[idx];
                fullValueCache[layer][currentStep][h][d] = valueDelta[idx];
            }
        }
    }

    int getCacheNonzeroCount() const {
        int count = 0;
        for (int i = 0; i < NUM_LAYERS; i++) {
            for (int j = 0; j < currentStep && j < MAX_DECODE_STEPS; j++) {
                for (int h = 0; h < NUM_HEADS; h++) {
                    for (int d = 0; d < HEAD_DIM; d++) {
                        if (fullKeyCache[i][j][h][d] != 0.0f) count++;
                        if (fullValueCache[i][j][h][d] != 0.0f) count++;
                    }
                }
            }
        }
        return count;
    }

    // Get tensor name (handles versioned Qnn_Tensor_t)
    static const char* tensorName(const Qnn_Tensor_t& t) {
        if (t.version == QNN_TENSOR_VERSION_2) return t.v2.name;
        return t.v1.name;
    }
    static uint32_t tensorId(const Qnn_Tensor_t& t) {
        if (t.version == QNN_TENSOR_VERSION_2) return t.v2.id;
        return t.v1.id;
    }
    static Qnn_TensorType_t tensorType(const Qnn_Tensor_t& t) {
        if (t.version == QNN_TENSOR_VERSION_2) return t.v2.type;
        return t.v1.type;
    }
    static Qnn_DataType_t tensorDataType(const Qnn_Tensor_t& t) {
        if (t.version == QNN_TENSOR_VERSION_2) return t.v2.dataType;
        return t.v1.dataType;
    }
    static uint32_t tensorRank(const Qnn_Tensor_t& t) {
        if (t.version == QNN_TENSOR_VERSION_2) return t.v2.rank;
        return t.v1.rank;
    }
    static uint32_t* tensorDims(const Qnn_Tensor_t& t) {
        if (t.version == QNN_TENSOR_VERSION_2) return t.v2.dimensions;
        return t.v1.dimensions;
    }
    static const Qnn_QuantizeParams_t& tensorQuantParams(const Qnn_Tensor_t& t) {
        if (t.version == QNN_TENSOR_VERSION_2) return t.v2.quantizeParams;
        return t.v1.quantizeParams;
    }

    static void logTensorEncoding(const char* tag, const Qnn_Tensor_t& t) {
        const char* name = tensorName(t);
        const auto dataType = tensorDataType(t);
        const auto rank = tensorRank(t);
        uint32_t* dims = tensorDims(t);
        const auto& qp = tensorQuantParams(t);

        // Build dimension string
        char dimStr[128] = {};
        int pos = 0;
        for (uint32_t d = 0; d < rank && d < 8; ++d) {
            pos += snprintf(dimStr + pos, sizeof(dimStr) - pos,
                           "%s%u", d > 0 ? "x" : "", dims[d]);
        }

        // Map encodingDefinition enum to string
        const char* encDefStr =
            (qp.encodingDefinition == QNN_DEFINITION_DEFINED) ? "DEFINED" :
            (qp.encodingDefinition == QNN_DEFINITION_IMPL_GENERATED) ? "IMPL_GENERATED" :
            "UNKNOWN";

        // Map quantizationEncoding enum to string
        const char* quantEncStr = "UNKNOWN";
        switch (qp.quantizationEncoding) {
            case QNN_QUANTIZATION_ENCODING_SCALE_OFFSET: quantEncStr = "SCALE_OFFSET"; break;
            case QNN_QUANTIZATION_ENCODING_AXIS_SCALE_OFFSET: quantEncStr = "AXIS_SCALE_OFFSET"; break;
            case QNN_QUANTIZATION_ENCODING_BW_SCALE_OFFSET: quantEncStr = "BW_SCALE_OFFSET"; break;
            case QNN_QUANTIZATION_ENCODING_BW_AXIS_SCALE_OFFSET: quantEncStr = "BW_AXIS_SCALE_OFFSET"; break;
            default: break;
        }

        // Map dataType enum to string and get bitwidth
        const char* dtypeStr = "UNKNOWN";
        int bitwidth = 0;
        switch (dataType) {
            case QNN_DATATYPE_FLOAT_32:          dtypeStr = "FLOAT32";  bitwidth = 32; break;
            case QNN_DATATYPE_FLOAT_16:          dtypeStr = "FLOAT16";  bitwidth = 16; break;
            case QNN_DATATYPE_UFIXED_POINT_16:   dtypeStr = "UFIXED16"; bitwidth = 16; break;
            case QNN_DATATYPE_SFIXED_POINT_16:   dtypeStr = "SFIXED16"; bitwidth = 16; break;
            case QNN_DATATYPE_INT_32:            dtypeStr = "INT32";    bitwidth = 32; break;
            case QNN_DATATYPE_UINT_32:           dtypeStr = "UINT32";   bitwidth = 32; break;
            case QNN_DATATYPE_INT_16:            dtypeStr = "INT16";    bitwidth = 16; break;
            case QNN_DATATYPE_UINT_16:           dtypeStr = "UINT16";   bitwidth = 16; break;
            case QNN_DATATYPE_INT_8:             dtypeStr = "INT8";     bitwidth = 8;  break;
            case QNN_DATATYPE_UINT_8:            dtypeStr = "UINT8";    bitwidth = 8;  break;
            default: break;
        }

        LOGI("[%s] tensor=%s dtype=%s(%dbit) dims=[%s] encDef=%s quantEnc=%s",
             tag, name ? name : "?", dtypeStr, bitwidth, dimStr, encDefStr, quantEncStr);

        if (qp.encodingDefinition == QNN_DEFINITION_DEFINED &&
            qp.quantizationEncoding == QNN_QUANTIZATION_ENCODING_SCALE_OFFSET) {
            const float scale = qp.scaleOffsetEncoding.scale;
            const int32_t offset = qp.scaleOffsetEncoding.offset;
            // Compute implied min/max from scale+offset for uint16 (0..65535)
            const float impliedMin = (0 + offset) * scale;
            const float impliedMax = (65535 + offset) * scale;
            LOGI("[%s] tensor=%s scale=%.10e offset=%d implied_range=[%.6f, %.6f]",
                 tag, name ? name : "?", scale, offset, impliedMin, impliedMax);
        } else if (qp.encodingDefinition == QNN_DEFINITION_DEFINED &&
                   qp.quantizationEncoding == QNN_QUANTIZATION_ENCODING_BW_SCALE_OFFSET) {
            const auto& bw = qp.bwScaleOffsetEncoding;
            LOGI("[%s] tensor=%s bw_scale=%.10e bw_offset=%d bw_bitwidth=%u",
                 tag, name ? name : "?", bw.scale, bw.offset, bw.bitwidth);
        } else if (qp.encodingDefinition == QNN_DEFINITION_IMPL_GENERATED) {
            LOGI("[%s] tensor=%s encoding is IMPL_GENERATED — HTP decides the actual encoding!",
                 tag, name ? name : "?");
        } else {
            LOGI("[%s] tensor=%s encoding not defined or not scale-offset (encDef=%d, quantEnc=%d)",
                 tag, name ? name : "?", (int)qp.encodingDefinition, (int)qp.quantizationEncoding);
        }
    }

    static size_t tensorElementCount(const Qnn_Tensor_t& t) {
        size_t count = 1;
        const uint32_t rank = tensorRank(t);
        uint32_t* dims = tensorDims(t);
        for (uint32_t i = 0; i < rank; ++i) {
            count *= static_cast<size_t>(dims[i]);
        }
        return count;
    }

    static size_t tensorElementSize(const Qnn_Tensor_t& t) {
        switch (tensorDataType(t)) {
            case QNN_DATATYPE_FLOAT_32:
            case QNN_DATATYPE_INT_32:
            case QNN_DATATYPE_UINT_32:
                return 4;
            case QNN_DATATYPE_FLOAT_16:
            case QNN_DATATYPE_UFIXED_POINT_16:
            case QNN_DATATYPE_SFIXED_POINT_16:
            case QNN_DATATYPE_UINT_16:
            case QNN_DATATYPE_INT_16:
                return 2;
            case QNN_DATATYPE_UFIXED_POINT_8:
            case QNN_DATATYPE_SFIXED_POINT_8:
            case QNN_DATATYPE_UINT_8:
            case QNN_DATATYPE_INT_8:
            case QNN_DATATYPE_BOOL_8:
                return 1;
            default:
                return sizeof(float);
        }
    }

    static float dequantizeValue(uint16_t raw, const Qnn_QuantizeParams_t& params) {
        if (params.encodingDefinition == QNN_DEFINITION_DEFINED &&
            params.quantizationEncoding == QNN_QUANTIZATION_ENCODING_SCALE_OFFSET) {
            const auto& scaleOffset = params.scaleOffsetEncoding;
            return (static_cast<int32_t>(raw) + scaleOffset.offset) * scaleOffset.scale;
        }
        return static_cast<float>(raw);
    }

    static uint16_t quantizeUfixed16(float value, const Qnn_QuantizeParams_t& params) {
        if (params.encodingDefinition == QNN_DEFINITION_DEFINED &&
            params.quantizationEncoding == QNN_QUANTIZATION_ENCODING_SCALE_OFFSET) {
            const auto& scaleOffset = params.scaleOffsetEncoding;
            if (scaleOffset.scale != 0.0f) {
                const float quantized = value / scaleOffset.scale - static_cast<float>(scaleOffset.offset);
                const float rounded = std::nearbyintf(quantized);
                const float clamped = std::clamp(
                    rounded,
                    0.0f,
                    static_cast<float>(std::numeric_limits<uint16_t>::max()));
                return static_cast<uint16_t>(clamped);
            }
        }
        const float clamped = std::clamp(
            std::nearbyintf(value),
            0.0f,
            static_cast<float>(std::numeric_limits<uint16_t>::max()));
        return static_cast<uint16_t>(clamped);
    }

    static void writeFloatInputToTensor(const float* src, size_t count, const Qnn_Tensor_t& tensor, void* dst) {
        if (!src || !dst) return;
        const size_t tensorCount = tensorElementCount(tensor);
        const size_t copyCount = std::min(count, tensorCount);

        switch (tensorDataType(tensor)) {
            case QNN_DATATYPE_FLOAT_32: {
                std::memset(dst, 0, tensorCount * sizeof(float));
                std::memcpy(dst, src, copyCount * sizeof(float));
                return;
            }
            case QNN_DATATYPE_UFIXED_POINT_16: {
                auto* out = reinterpret_cast<uint16_t*>(dst);
                const auto& params = tensorQuantParams(tensor);
                std::fill(out, out + tensorCount, static_cast<uint16_t>(0));
                for (size_t i = 0; i < copyCount; ++i) {
                    out[i] = quantizeUfixed16(src[i], params);
                }
                return;
            }
            default: {
                std::memset(dst, 0, tensorCount * tensorElementSize(tensor));
                return;
            }
        }
    }

    static void writeIntInputToTensor(const int64_t* src, size_t count, const Qnn_Tensor_t& tensor, void* dst) {
        if (!src || !dst) return;
        const size_t tensorCount = tensorElementCount(tensor);
        const size_t copyCount = std::min(count, tensorCount);

        switch (tensorDataType(tensor)) {
            case QNN_DATATYPE_INT_32: {
                auto* out = reinterpret_cast<int32_t*>(dst);
                std::fill(out, out + tensorCount, 0);
                for (size_t i = 0; i < copyCount; ++i) {
                    out[i] = static_cast<int32_t>(src[i]);
                }
                return;
            }
            case QNN_DATATYPE_UINT_32: {
                auto* out = reinterpret_cast<uint32_t*>(dst);
                std::fill(out, out + tensorCount, 0U);
                for (size_t i = 0; i < copyCount; ++i) {
                    out[i] = static_cast<uint32_t>(std::max<int64_t>(0, src[i]));
                }
                return;
            }
            case QNN_DATATYPE_INT_16: {
                auto* out = reinterpret_cast<int16_t*>(dst);
                std::fill(out, out + tensorCount, static_cast<int16_t>(0));
                for (size_t i = 0; i < copyCount; ++i) {
                    out[i] = static_cast<int16_t>(src[i]);
                }
                return;
            }
            case QNN_DATATYPE_UINT_16: {
                auto* out = reinterpret_cast<uint16_t*>(dst);
                std::fill(out, out + tensorCount, static_cast<uint16_t>(0));
                for (size_t i = 0; i < copyCount; ++i) {
                    out[i] = static_cast<uint16_t>(std::max<int64_t>(0, src[i]));
                }
                return;
            }
            case QNN_DATATYPE_INT_8: {
                auto* out = reinterpret_cast<int8_t*>(dst);
                std::fill(out, out + tensorCount, static_cast<int8_t>(0));
                for (size_t i = 0; i < copyCount; ++i) {
                    out[i] = static_cast<int8_t>(src[i]);
                }
                return;
            }
            case QNN_DATATYPE_UINT_8:
            case QNN_DATATYPE_BOOL_8: {
                auto* out = reinterpret_cast<uint8_t*>(dst);
                std::fill(out, out + tensorCount, static_cast<uint8_t>(0));
                for (size_t i = 0; i < copyCount; ++i) {
                    out[i] = static_cast<uint8_t>(src[i] != 0 ? 1 : 0);
                }
                return;
            }
            default: {
                auto* out = reinterpret_cast<int64_t*>(dst);
                std::fill(out, out + tensorCount, static_cast<int64_t>(0));
                for (size_t i = 0; i < copyCount; ++i) {
                    out[i] = src[i];
                }
                return;
            }
        }
    }

    static void readTensorToFloat(const Qnn_Tensor_t& tensor, const void* src, float* dst, size_t count) {
        if (!src || !dst) return;

        switch (tensorDataType(tensor)) {
            case QNN_DATATYPE_FLOAT_32: {
                std::memcpy(dst, src, count * sizeof(float));
                return;
            }
            case QNN_DATATYPE_UFIXED_POINT_16: {
                const auto* in = reinterpret_cast<const uint16_t*>(src);
                const auto& params = tensorQuantParams(tensor);
                for (size_t i = 0; i < count; ++i) {
                    dst[i] = dequantizeValue(in[i], params);
                }
                return;
            }
            default: {
                std::fill(dst, dst + count, 0.0f);
                return;
            }
        }
    }

    // Find tensor by name in graph info
    Qnn_Tensor_t* findInputTensor(const char* name) {
        if (!graphsInfo || graphsCount == 0) return nullptr;
        auto* gi = graphsInfo[0];
        for (uint32_t i = 0; i < gi->numInputTensors; i++) {
            const char* n = tensorName(gi->inputTensors[i]);
            if (n && strcmp(n, name) == 0) return &gi->inputTensors[i];
        }
        return nullptr;
    }

    Qnn_Tensor_t* findOutputTensor(const char* name) {
        if (!graphsInfo || graphsCount == 0) return nullptr;
        auto* gi = graphsInfo[0];
        for (uint32_t i = 0; i < gi->numOutputTensors; i++) {
            const char* n = tensorName(gi->outputTensors[i]);
            if (n && strcmp(n, name) == 0) return &gi->outputTensors[i];
        }
        return nullptr;
    }

    void logBinaryInfo(const QnnSystemContext_BinaryInfo_t* binaryInfo) {
        if (!binaryInfo) return;

        if (binaryInfo->version == QNN_SYSTEM_CONTEXT_BINARY_INFO_VERSION_1) {
            const auto& info = binaryInfo->contextBinaryInfoV1;
            LOGI("[Path2] BinaryInfo V1: numGraphs=%u numContextTensors=%u",
                 info.numGraphs, info.numContextTensors);

            for (uint32_t i = 0; i < info.numContextTensors; i++) {
                const char* name = tensorName(info.contextTensors[i]);
                if (!name) continue;
                if (strncmp(name, "cache_key_", 10) == 0 ||
                    strncmp(name, "cache_value_", 12) == 0) {
                    logTensorEncoding("Path2-ctx", info.contextTensors[i]);
                }
            }

            for (uint32_t g = 0; g < info.numGraphs; g++) {
                const auto& graph = info.graphs[g];
                if (graph.version == QNN_SYSTEM_CONTEXT_GRAPH_INFO_VERSION_1) {
                    const auto& gi = graph.graphInfoV1;
                    LOGI("[Path2] Graph '%s': %u inputs, %u outputs",
                         gi.graphName ? gi.graphName : "?",
                         gi.numGraphInputs, gi.numGraphOutputs);
                    for (uint32_t i = 0; i < gi.numGraphInputs; i++) {
                        const char* tname = tensorName(gi.graphInputs[i]);
                        if (!tname) continue;
                        if (strncmp(tname, "cache_key_", 10) == 0 ||
                            strncmp(tname, "cache_value_", 12) == 0) {
                            logTensorEncoding("Path2-graph-in", gi.graphInputs[i]);
                        }
                    }
                    for (uint32_t i = 0; i < gi.numGraphOutputs; i++) {
                        const char* tname = tensorName(gi.graphOutputs[i]);
                        if (!tname) continue;
                        if (strncmp(tname, "key_delta_", 10) == 0 ||
                            strncmp(tname, "value_delta_", 12) == 0) {
                            logTensorEncoding("Path2-graph-out", gi.graphOutputs[i]);
                        }
                    }
                } else if (graph.version == QNN_SYSTEM_CONTEXT_GRAPH_INFO_VERSION_2) {
                    const auto& gi = graph.graphInfoV2;
                    LOGI("[Path2] Graph V2 '%s': %u inputs, %u outputs",
                         gi.graphName ? gi.graphName : "?",
                         gi.numGraphInputs, gi.numGraphOutputs);
                    for (uint32_t i = 0; i < gi.numGraphInputs; i++) {
                        const char* tname = tensorName(gi.graphInputs[i]);
                        if (!tname) continue;
                        if (strncmp(tname, "cache_key_", 10) == 0 ||
                            strncmp(tname, "cache_value_", 12) == 0) {
                            logTensorEncoding("Path2-graph-in", gi.graphInputs[i]);
                        }
                    }
                } else if (graph.version == QNN_SYSTEM_CONTEXT_GRAPH_INFO_VERSION_3) {
                    const auto& gi = graph.graphInfoV3;
                    LOGI("[Path2] Graph V3 '%s': %u inputs, %u outputs",
                         gi.graphName ? gi.graphName : "?",
                         gi.numGraphInputs, gi.numGraphOutputs);
                    for (uint32_t i = 0; i < gi.numGraphInputs; i++) {
                        const char* tname = tensorName(gi.graphInputs[i]);
                        if (!tname) continue;
                        if (strncmp(tname, "cache_key_", 10) == 0 ||
                            strncmp(tname, "cache_value_", 12) == 0) {
                            logTensorEncoding("Path2-graph-in", gi.graphInputs[i]);
                        }
                    }
                }
            }
        } else if (binaryInfo->version == QNN_SYSTEM_CONTEXT_BINARY_INFO_VERSION_2) {
            const auto& info = binaryInfo->contextBinaryInfoV2;
            LOGI("[Path2] BinaryInfo V2: numGraphs=%u numContextTensors=%u",
                 info.numGraphs, info.numContextTensors);

            for (uint32_t i = 0; i < info.numContextTensors; i++) {
                const char* name = tensorName(info.contextTensors[i]);
                if (!name) continue;
                if (strncmp(name, "cache_key_", 10) == 0 ||
                    strncmp(name, "cache_value_", 12) == 0) {
                    logTensorEncoding("Path2-ctx", info.contextTensors[i]);
                }
            }

            for (uint32_t g = 0; g < info.numGraphs; g++) {
                const auto& graph = info.graphs[g];
                if (graph.version == QNN_SYSTEM_CONTEXT_GRAPH_INFO_VERSION_1) {
                    const auto& gi = graph.graphInfoV1;
                    for (uint32_t i = 0; i < gi.numGraphInputs; i++) {
                        const char* tname = tensorName(gi.graphInputs[i]);
                        if (!tname) continue;
                        if (strncmp(tname, "cache_key_", 10) == 0 ||
                            strncmp(tname, "cache_value_", 12) == 0) {
                            logTensorEncoding("Path2-graph-in", gi.graphInputs[i]);
                        }
                    }
                } else if (graph.version == QNN_SYSTEM_CONTEXT_GRAPH_INFO_VERSION_2) {
                    const auto& gi = graph.graphInfoV2;
                    for (uint32_t i = 0; i < gi.numGraphInputs; i++) {
                        const char* tname = tensorName(gi.graphInputs[i]);
                        if (!tname) continue;
                        if (strncmp(tname, "cache_key_", 10) == 0 ||
                            strncmp(tname, "cache_value_", 12) == 0) {
                            logTensorEncoding("Path2-graph-in", gi.graphInputs[i]);
                        }
                    }
                } else if (graph.version == QNN_SYSTEM_CONTEXT_GRAPH_INFO_VERSION_3) {
                    const auto& gi = graph.graphInfoV3;
                    for (uint32_t i = 0; i < gi.numGraphInputs; i++) {
                        const char* tname = tensorName(gi.graphInputs[i]);
                        if (!tname) continue;
                        if (strncmp(tname, "cache_key_", 10) == 0 ||
                            strncmp(tname, "cache_value_", 12) == 0) {
                            logTensorEncoding("Path2-graph-in", gi.graphInputs[i]);
                        }
                    }
                }
            }
        } else if (binaryInfo->version == QNN_SYSTEM_CONTEXT_BINARY_INFO_VERSION_3) {
            const auto& info = binaryInfo->contextBinaryInfoV3;
            LOGI("[Path2] BinaryInfo V3: numGraphs=%u numContextTensors=%u",
                 info.numGraphs, info.numContextTensors);

            for (uint32_t i = 0; i < info.numContextTensors; i++) {
                const char* name = tensorName(info.contextTensors[i]);
                if (!name) continue;
                if (strncmp(name, "cache_key_", 10) == 0 ||
                    strncmp(name, "cache_value_", 12) == 0) {
                    logTensorEncoding("Path2-ctx", info.contextTensors[i]);
                }
            }

            for (uint32_t g = 0; g < info.numGraphs; g++) {
                const auto& graph = info.graphs[g];
                if (graph.version == QNN_SYSTEM_CONTEXT_GRAPH_INFO_VERSION_1) {
                    const auto& gi = graph.graphInfoV1;
                    for (uint32_t i = 0; i < gi.numGraphInputs; i++) {
                        const char* tname = tensorName(gi.graphInputs[i]);
                        if (!tname) continue;
                        if (strncmp(tname, "cache_key_", 10) == 0 ||
                            strncmp(tname, "cache_value_", 12) == 0) {
                            logTensorEncoding("Path2-graph-in", gi.graphInputs[i]);
                        }
                    }
                } else if (graph.version == QNN_SYSTEM_CONTEXT_GRAPH_INFO_VERSION_2) {
                    const auto& gi = graph.graphInfoV2;
                    for (uint32_t i = 0; i < gi.numGraphInputs; i++) {
                        const char* tname = tensorName(gi.graphInputs[i]);
                        if (!tname) continue;
                        if (strncmp(tname, "cache_key_", 10) == 0 ||
                            strncmp(tname, "cache_value_", 12) == 0) {
                            logTensorEncoding("Path2-graph-in", gi.graphInputs[i]);
                        }
                    }
                } else if (graph.version == QNN_SYSTEM_CONTEXT_GRAPH_INFO_VERSION_3) {
                    const auto& gi = graph.graphInfoV3;
                    for (uint32_t i = 0; i < gi.numGraphInputs; i++) {
                        const char* tname = tensorName(gi.graphInputs[i]);
                        if (!tname) continue;
                        if (strncmp(tname, "cache_key_", 10) == 0 ||
                            strncmp(tname, "cache_value_", 12) == 0) {
                            logTensorEncoding("Path2-graph-in", gi.graphInputs[i]);
                        }
                    }
                }
            }
        } else {
            LOGW("[Path2] Unknown BinaryInfo version: %d", (int)binaryInfo->version);
        }
    }

    void release() {
        convSession.reset();
        encoderSession.reset();
        ortSessionOptions.reset();
        ortEnv.reset();
        if (decoderGraph && qnnInterface.graphFinalize) {
            // graph doesn't need explicit free in QNN
        }
        if (contextHandle && qnnInterface.contextFree) {
            qnnInterface.contextFree(contextHandle, nullptr);
            contextHandle = nullptr;
        }
        if (deviceHandle && qnnInterface.deviceFree) {
            qnnInterface.deviceFree(deviceHandle);
            deviceHandle = nullptr;
        }
        if (backendHandle && qnnInterface.backendFree) {
            qnnInterface.backendFree(backendHandle);
            backendHandle = nullptr;
        }
        if (logHandle && qnnInterface.logFree) {
            qnnInterface.logFree(logHandle);
            logHandle = nullptr;
        }
        if (graphsInfo && freeGraphInfoFn) {
            freeGraphInfoFn(&graphsInfo, graphsCount);
            graphsInfo = nullptr;
            graphsCount = 0;
        }
        decoderGraph = nullptr;
    }
};

static std::vector<std::string> GetSessionNames(Ort::Session& session, bool input) {
    std::vector<std::string> names;
    Ort::AllocatorWithDefaultOptions allocator;
    const size_t count = input ? session.GetInputCount() : session.GetOutputCount();
    names.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        Ort::AllocatedStringPtr name = input
            ? session.GetInputNameAllocated(i, allocator)
            : session.GetOutputNameAllocated(i, allocator);
        names.emplace_back(name ? name.get() : "");
    }
    return names;
}

static std::vector<const char*> ToNamePtrs(const std::vector<std::string>& names) {
    std::vector<const char*> out;
    out.reserve(names.size());
    for (const auto& name : names) {
        out.push_back(name.c_str());
    }
    return out;
}

Qwen3QnnBackend::Qwen3QnnBackend() : m_impl(new Impl()) {}

Qwen3QnnBackend::~Qwen3QnnBackend() {
    release();
    delete m_impl;
}

bool Qwen3QnnBackend::init(const std::string& modelDir, const std::string& qnnLibDir) {
    using Clock = std::chrono::steady_clock;
    LOGI("=== Qwen3 QNN Backend Init ===");
    LOGI("Model dir: %s", modelDir.c_str());
    LOGI("QNN lib dir: %s", qnnLibDir.c_str());
    const auto initStart = Clock::now();

    std::string cpuModelDir = modelDir;
    constexpr const char kQnnSuffix[] = "-qnn";
    if (cpuModelDir.size() > strlen(kQnnSuffix) &&
        cpuModelDir.compare(cpuModelDir.size() - strlen(kQnnSuffix), strlen(kQnnSuffix), kQnnSuffix) == 0) {
        cpuModelDir.resize(cpuModelDir.size() - strlen(kQnnSuffix));
    }

    m_impl->convFrontendPath = cpuModelDir + "/conv_frontend.onnx";
    m_impl->encoderPath = cpuModelDir + "/encoder.int8.onnx";
    m_impl->tokenizerPath = modelDir + "/tokenizer";

    // Check required decoder library
    std::string externalDecoderLibPath = modelDir + "/decoder-w4/libmodel.so";
    std::string runtimeDecoderLibPath = qnnLibDir.empty() ? "" : (qnnLibDir + "/libmodel.so");
    std::string decoderLibPath = runtimeDecoderLibPath.empty() ? externalDecoderLibPath : runtimeDecoderLibPath;

    LOGI("Decoder lib: %s", decoderLibPath.c_str());
    LOGI("CPU conv frontend: %s", m_impl->convFrontendPath.c_str());
    LOGI("CPU encoder: %s", m_impl->encoderPath.c_str());

    try {
        m_impl->ortEnv = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "pocketvoice-qwen3");
        m_impl->ortSessionOptions = std::make_unique<Ort::SessionOptions>();
        m_impl->ortSessionOptions->SetIntraOpNumThreads(1);
        m_impl->ortSessionOptions->SetInterOpNumThreads(1);
        m_impl->ortSessionOptions->SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        m_impl->convSession = std::make_unique<Ort::Session>(
            *m_impl->ortEnv, m_impl->convFrontendPath.c_str(), *m_impl->ortSessionOptions);
        m_impl->encoderSession = std::make_unique<Ort::Session>(
            *m_impl->ortEnv, m_impl->encoderPath.c_str(), *m_impl->ortSessionOptions);
        m_impl->convInputNames = GetSessionNames(*m_impl->convSession, true);
        m_impl->convOutputNames = GetSessionNames(*m_impl->convSession, false);
        m_impl->convInputNamePtrs = ToNamePtrs(m_impl->convInputNames);
        m_impl->convOutputNamePtrs = ToNamePtrs(m_impl->convOutputNames);
        m_impl->encoderInputNames = GetSessionNames(*m_impl->encoderSession, true);
        m_impl->encoderOutputNames = GetSessionNames(*m_impl->encoderSession, false);
        m_impl->encoderInputNamePtrs = ToNamePtrs(m_impl->encoderInputNames);
        m_impl->encoderOutputNamePtrs = ToNamePtrs(m_impl->encoderOutputNames);
        LOGI("ORT conv/encoder sessions initialized");
    } catch (const Ort::Exception& e) {
        LOGE("Failed to init ORT conv/encoder: %s", e.what());
        release();
        return false;
    }

    // ============================================================
    // Step 1: Load QNN HTP backend library
    // ============================================================
    std::string qnnHtpPath = qnnLibDir + "/libQnnHtp.so";
    m_impl->qnnHtpLib = dlopen(qnnHtpPath.c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (!m_impl->qnnHtpLib) {
        LOGE("Failed to load libQnnHtp.so: %s", dlerror());
        return false;
    }
    LOGI("Loaded libQnnHtp.so");

    // ============================================================
    // Step 2: Get QNN interface via QnnInterface_getProviders
    // ============================================================
    typedef Qnn_ErrorHandle_t (*QnnInterface_getProvidersFn_t)(
        const QnnInterface_t*** providerList, uint32_t* numProviders);

    auto getProviders = (QnnInterface_getProvidersFn_t)dlsym(m_impl->qnnHtpLib, "QnnInterface_getProviders");
    if (!getProviders) {
        LOGE("Failed to find QnnInterface_getProviders: %s", dlerror());
        release();
        return false;
    }

    const QnnInterface_t** providerList = nullptr;
    uint32_t numProviders = 0;
    Qnn_ErrorHandle_t err = getProviders(&providerList, &numProviders);
    if (err != QNN_SUCCESS || numProviders == 0 || !providerList) {
        LOGE("QnnInterface_getProviders failed: err=%lu, numProviders=%u", (unsigned long)err, numProviders);
        release();
        return false;
    }

    // Use the first provider
    m_impl->qnnInterfaceHandle = providerList[0];
    m_impl->qnnInterface = m_impl->qnnInterfaceHandle->QNN_INTERFACE_VER_NAME;

    LOGI("QNN interface obtained: backend=%u, provider=%s",
         m_impl->qnnInterfaceHandle->backendId,
         m_impl->qnnInterfaceHandle->providerName ? m_impl->qnnInterfaceHandle->providerName : "null");

    // Verify critical function pointers
    if (!m_impl->qnnInterface.backendCreate || !m_impl->qnnInterface.backendFree) {
        LOGE("backendCreate or backendFree is null");
        release();
        return false;
    }
    if (!m_impl->qnnInterface.contextCreate || !m_impl->qnnInterface.contextFree) {
        LOGE("contextCreate or contextFree is null");
        release();
        return false;
    }
    if (!m_impl->qnnInterface.graphExecute) {
        LOGE("graphExecute is null");
        release();
        return false;
    }
    LOGI("All critical QNN function pointers verified");

    // ============================================================
    // Step 3: Load QNN System library
    // ============================================================
    std::string qnnSystemPath = qnnLibDir + "/libQnnSystem.so";
    m_impl->qnnSystemLib = dlopen(qnnSystemPath.c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (!m_impl->qnnSystemLib) {
        LOGW("Failed to load libQnnSystem.so: %s (non-fatal)", dlerror());
    } else {
        typedef Qnn_ErrorHandle_t (*QnnSystemInterface_getProvidersFn_t)(
            const QnnSystemInterface_t*** providerList, uint32_t* numProviders);

        auto getSystemProviders = (QnnSystemInterface_getProvidersFn_t)dlsym(
            m_impl->qnnSystemLib, "QnnSystemInterface_getProviders");
        if (getSystemProviders) {
            const QnnSystemInterface_t** sysProviderList = nullptr;
            uint32_t sysNumProviders = 0;
            err = getSystemProviders(&sysProviderList, &sysNumProviders);
            if (err == QNN_SUCCESS && sysNumProviders > 0 && sysProviderList) {
                m_impl->qnnSystemInterfaceHandle = sysProviderList[0];
                m_impl->qnnSystemInterface = m_impl->qnnSystemInterfaceHandle->QNN_SYSTEM_INTERFACE_VER_NAME;
                LOGI("QNN System interface obtained");
            } else {
                LOGW("QnnSystemInterface_getProviders failed: err=%lu", (unsigned long)err);
            }
        }
    }

    // ============================================================
    // Step 4: Initialize QNN logging
    // ============================================================
    if (m_impl->qnnInterface.logCreate) {
        err = m_impl->qnnInterface.logCreate(
            qnnLogCallback, QNN_LOG_LEVEL_INFO, &m_impl->logHandle);
        if (err != QNN_SUCCESS) {
            LOGW("QnnLog_create failed: %lu (non-fatal)", (unsigned long)err);
        } else {
            LOGI("QNN logging initialized");
        }
    }

    // ============================================================
    // Step 5: Create QNN backend
    // ============================================================
    err = m_impl->qnnInterface.backendCreate(
        m_impl->logHandle, nullptr, &m_impl->backendHandle);
    if (err != QNN_SUCCESS) {
        LOGE("QnnBackend_create failed: %lu", (unsigned long)err);
        release();
        return false;
    }
    LOGI("QNN backend created");

    // ============================================================
    // Step 6: Create QNN device
    // ============================================================
    if (m_impl->qnnInterface.deviceCreate) {
        err = m_impl->qnnInterface.deviceCreate(
            m_impl->logHandle, nullptr, &m_impl->deviceHandle);
        if (err != QNN_SUCCESS && err != QNN_DEVICE_ERROR_UNSUPPORTED_FEATURE) {
            LOGE("QnnDevice_create failed: %lu", (unsigned long)err);
            release();
            return false;
        }
        LOGI("QNN device created (err=%lu)", (unsigned long)err);
    }

    // ============================================================
    // Step 7: Create QNN context
    // ============================================================
    err = m_impl->qnnInterface.contextCreate(
        m_impl->backendHandle, m_impl->deviceHandle, nullptr, &m_impl->contextHandle);
    if (err != QNN_SUCCESS) {
        LOGE("QnnContext_create failed: %lu", (unsigned long)err);
        release();
        return false;
    }
    LOGI("QNN context created");

    // ============================================================
    // Step 8: Load decoder libmodel.so and get composeGraphs
    // ============================================================
    m_impl->decoderLib = dlopen(decoderLibPath.c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (!m_impl->decoderLib) {
        LOGE("Failed to load decoder libmodel.so: %s", dlerror());
        release();
        return false;
    }
    LOGI("Decoder libmodel.so loaded");

    // The generated libmodel.so exports QnnModel_composeGraphs and QnnModel_freeGraphsInfo
    m_impl->composeGraphsFn = (ComposeGraphsFnHandleType_t)dlsym(m_impl->decoderLib, "QnnModel_composeGraphs");
    if (!m_impl->composeGraphsFn) {
        // Try alternate name
        m_impl->composeGraphsFn = (ComposeGraphsFnHandleType_t)dlsym(m_impl->decoderLib, "composeGraphs");
    }
    if (!m_impl->composeGraphsFn) {
        LOGE("Failed to find QnnModel_composeGraphs in decoder libmodel.so: %s", dlerror());
        release();
        return false;
    }
    LOGI("QnnModel_composeGraphs function found");

    m_impl->freeGraphInfoFn = (FreeGraphInfoFnHandleType_t)dlsym(m_impl->decoderLib, "QnnModel_freeGraphsInfo");
    if (!m_impl->freeGraphInfoFn) {
        m_impl->freeGraphInfoFn = (FreeGraphInfoFnHandleType_t)dlsym(m_impl->decoderLib, "freeGraphInfo");
    }

    // ============================================================
    // Step 9: Compose graphs from decoder model
    // ============================================================
    LOGI("composeGraphs begin");
    const auto composeStart = Clock::now();
    qnn_wrapper_api::ModelError_t modelErr = m_impl->composeGraphsFn(
        m_impl->backendHandle,
        m_impl->qnnInterface,
        m_impl->contextHandle,
        nullptr,  // graphConfigsInfo
        0,        // graphConfigsInfoCount
        &m_impl->graphsInfo,
        &m_impl->graphsCount,
        false,    // debug
        qnnLogCallback,
        QNN_LOG_LEVEL_INFO);

    if (modelErr != qnn_wrapper_api::MODEL_NO_ERROR) {
        LOGE("composeGraphs failed: %d", modelErr);
        release();
        return false;
    }
    const auto composeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - composeStart).count();
    LOGI("composeGraphs succeeded: %u graph(s)", m_impl->graphsCount);
    LOGI("composeGraphs end, elapsed=%lld ms", static_cast<long long>(composeMs));

    if (m_impl->graphsCount == 0 || !m_impl->graphsInfo) {
        LOGE("No graphs composed");
        release();
        return false;
    }

    // ============================================================
    // Step 10: Finalize the decoder graph
    // ============================================================
    for (uint32_t g = 0; g < m_impl->graphsCount; g++) {
        auto* gi = m_impl->graphsInfo[g];
        if (!gi || !gi->graph) continue;

        LOGI("graphFinalize begin: index=%u name=%s", g, gi->graphName ? gi->graphName : "?");
        const auto finalizeStart = Clock::now();
        err = m_impl->qnnInterface.graphFinalize(gi->graph, nullptr, nullptr);
        if (err != QNN_SUCCESS) {
            LOGE("graphFinalize failed for graph '%s': %lu",
                 gi->graphName ? gi->graphName : "?", (unsigned long)err);
            release();
            return false;
        }
        const auto finalizeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - finalizeStart).count();
        LOGI("Graph '%s' finalized (%u inputs, %u outputs)",
             gi->graphName ? gi->graphName : "?",
             gi->numInputTensors, gi->numOutputTensors);
        LOGI("graphFinalize end: index=%u elapsed=%lld ms",
             g, static_cast<long long>(finalizeMs));

        // Log input tensor names and quantization parameters
        for (uint32_t i = 0; i < gi->numInputTensors; i++) {
            const char* tn = Impl::tensorName(gi->inputTensors[i]);
            const auto& qp = Impl::tensorQuantParams(gi->inputTensors[i]);
            if (qp.encodingDefinition == QNN_DEFINITION_DEFINED &&
                qp.quantizationEncoding == QNN_QUANTIZATION_ENCODING_SCALE_OFFSET) {
                LOGI("  input[%u]: %s (id=%u, type=%d, dtype=%d, scale=%.10e, offset=%d)",
                     i, tn ? tn : "?",
                     Impl::tensorId(gi->inputTensors[i]),
                     (int)Impl::tensorType(gi->inputTensors[i]),
                     (int)Impl::tensorDataType(gi->inputTensors[i]),
                     qp.scaleOffsetEncoding.scale,
                     qp.scaleOffsetEncoding.offset);
            } else {
                LOGI("  input[%u]: %s (id=%u, type=%d, dtype=%d)",
                     i, tn ? tn : "?",
                     Impl::tensorId(gi->inputTensors[i]),
                     (int)Impl::tensorType(gi->inputTensors[i]),
                     (int)Impl::tensorDataType(gi->inputTensors[i]));
            }
        }
        // Log output tensor names
        for (uint32_t i = 0; i < gi->numOutputTensors; i++) {
            const char* tn = Impl::tensorName(gi->outputTensors[i]);
            LOGI("  output[%u]: %s (id=%u, type=%d)",
                 i, tn ? tn : "?",
                 Impl::tensorId(gi->outputTensors[i]),
                 (int)Impl::tensorType(gi->outputTensors[i]));
        }
    }

    // Use the first graph as decoder graph
    LOGI("Selecting decoder graph handle");
    m_impl->decoderGraph = m_impl->graphsInfo[0]->graph;

    // ============================================================
    // Step 11: Allocate KV cache
    // ============================================================
    LOGI("Allocating KV cache");
    m_impl->allocateCache();
    LOGI("Resetting KV cache");
    m_impl->resetCache();

    // ============================================================
    // Gate A: Runtime Encoding Audit
    // ============================================================
    LOGI("Running Gate A: Runtime Encoding Audit");
    auditRuntimeEncodings();

    m_initialized = true;
    const auto initMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - initStart).count();
    LOGI("=== Qwen3 QNN backend initialized OK ===");
    LOGI("Qwen3 QNN init total elapsed=%lld ms", static_cast<long long>(initMs));
    LOGI("Backend: qwen3_asr_qnn (HTP)");
    return true;
}

bool Qwen3QnnBackend::decodeStep(int inputToken, const float* audioFeatures,
                                  size_t audioFeatureSize, float* outputLogits) {
    if (!m_initialized || !m_impl->decoderGraph) {
        LOGE("Backend not initialized");
        return false;
    }

    if (!outputLogits) {
        LOGE("outputLogits is null");
        return false;
    }

    // 0-indexed position: step 0 → position 0 (past KV masked by position_scalar=0),
    // step 1+ → position > 0 (past KV unmasked). Matches sherpa-onnx cache_position semantics.
    const int position = m_impl->currentStep;
    LOGI("=== Decode step %d (position=%d), input token: %d ===", m_impl->currentStep, position, inputToken);

    auto* gi = m_impl->graphsInfo[0];
    if (!gi) {
        LOGE("No graph info");
        return false;
    }

    // Prepare input buffers
    // input_ids: [1,1] int32
    int64_t inputIds[1] = {static_cast<int64_t>(inputToken)};
    // attention_mask: [1,1]
    int64_t attentionMask[1] = {1};
    // position_scalar: [1]
    int64_t positionScalar[1] = {static_cast<int64_t>(position)};
    // audio_features: [1,65,1024] float (quantized ufixed16 in QNN, but we provide float)
    // rope_emb: [1,64] float
    // attention_bias: [1,1] float

    // Prepare rope_emb: position * inv_freq
    std::vector<float> ropeEmb(ROPE_FREQ_DIM, 0.0f);
    for (int i = 0; i < ROPE_FREQ_DIM; ++i) {
        ropeEmb[i] = static_cast<float>(position) * kRopeInvFreq[i];
    }

    // Prepare attention_bias (causal mask)
    std::vector<float> attentionBias(1, 0.0f);

    // Prepare KV cache windows for each layer
    std::vector<std::vector<float>> keyWindows(NUM_LAYERS, std::vector<float>(W * NUM_HEADS * HEAD_DIM, 0.0f));
    std::vector<std::vector<float>> valueWindows(NUM_LAYERS, std::vector<float>(W * NUM_HEADS * HEAD_DIM, 0.0f));
    for (int i = 0; i < NUM_LAYERS; i++) {
        m_impl->prepareWindow(keyWindows[i].data(), i, true);
        m_impl->prepareWindow(valueWindows[i].data(), i, false);
    }

    LOGI("Cache state: step=%d position=%d nonzero=%d", m_impl->currentStep, position, m_impl->getCacheNonzeroCount());

    // Gate 2: dump all inputs on the first few steps for reference alignment
    if (m_impl->currentStep <= 5) {
        LOGI("[DIAG-gate2] step=%d input_ids=%d attention_mask=1 position_scalar=%d",
             m_impl->currentStep, inputToken, position);
        LOGI("[DIAG-gate2] step=%d rope_emb[0..4]=%.6f %.6f %.6f %.6f %.6f",
             m_impl->currentStep, ropeEmb[0], ropeEmb[1], ropeEmb[2], ropeEmb[3], ropeEmb[4]);
        LOGI("[DIAG-gate2] step=%d attention_bias=%.6f", m_impl->currentStep, attentionBias[0]);
        if (audioFeatures && audioFeatureSize > 0) {
            float afMin = std::numeric_limits<float>::max();
            float afMax = std::numeric_limits<float>::lowest();
            float afSum = 0.0f;
            size_t afNonzero = 0;
            for (size_t i = 0; i < audioFeatureSize; ++i) {
                afMin = std::min(afMin, audioFeatures[i]);
                afMax = std::max(afMax, audioFeatures[i]);
                afSum += audioFeatures[i];
                if (audioFeatures[i] != 0.0f) ++afNonzero;
            }
            LOGI("[DIAG-gate2] step=%d audio_features: count=%zu min=%.6f max=%.6f sum=%.2f nonzero=%zu",
                 m_impl->currentStep, audioFeatureSize, afMin, afMax, afSum, afNonzero);
        } else {
            LOGI("[DIAG-gate2] step=%d audio_features: null/empty", m_impl->currentStep);
        }
        // Dump cache_key_0 / cache_value_0 window summary
        {
            float ckMin = std::numeric_limits<float>::max();
            float ckMax = std::numeric_limits<float>::lowest();
            float cvMin = std::numeric_limits<float>::max();
            float cvMax = std::numeric_limits<float>::lowest();
            size_t ckNonzero = 0, cvNonzero = 0;
            const size_t windowElems = static_cast<size_t>(W) * NUM_HEADS * HEAD_DIM;
            for (size_t i = 0; i < windowElems; ++i) {
                if (keyWindows[0][i] != 0.0f) { ++ckNonzero; ckMin = std::min(ckMin, keyWindows[0][i]); ckMax = std::max(ckMax, keyWindows[0][i]); }
                if (valueWindows[0][i] != 0.0f) { ++cvNonzero; cvMin = std::min(cvMin, valueWindows[0][i]); cvMax = std::max(cvMax, valueWindows[0][i]); }
            }
            LOGI("[DIAG-gate2] step=%d cache_key_0_win: nonzero=%zu/%zu min=%.6f max=%.6f",
                 m_impl->currentStep, ckNonzero, windowElems,
                 ckNonzero > 0 ? ckMin : 0.0f, ckNonzero > 0 ? ckMax : 0.0f);
            LOGI("[DIAG-gate2] step=%d cache_value_0_win: nonzero=%zu/%zu min=%.6f max=%.6f",
                 m_impl->currentStep, cvNonzero, windowElems,
                 cvNonzero > 0 ? cvMin : 0.0f, cvNonzero > 0 ? cvMax : 0.0f);
        }
        // Record window parameters for Gate 3
        const int windowStart = std::max(0, m_impl->currentStep - W);
        const int windowCount = std::min(W, m_impl->currentStep);
        LOGI("[DIAG-gate2] step=%d window_start=%d window_count=%d", m_impl->currentStep, windowStart, windowCount);
    }

    // ============================================================
    // Set up input tensors by name
    // ============================================================
    // We need to match the tensor IDs from the graph info
    // The tensors were created during composeGraphs, so we need to use the same IDs

    // For now, we'll create temporary tensor arrays for graphExecute
    // The key insight: we need to match the tensor IDs that were assigned during composeGraphs

    std::vector<std::vector<uint8_t>> inputBuffers(gi->numInputTensors);

    // Build input tensor array from graph info
    // Qnn_Tensor_t is versioned: v1 or v2. We set clientBuf on the appropriate version.
    std::vector<Qnn_Tensor_t> inputs(gi->numInputTensors);
    for (uint32_t i = 0; i < gi->numInputTensors; i++) {
        inputs[i] = gi->inputTensors[i];  // copy full struct (including version)
        const char* name = Impl::tensorName(inputs[i]);
        if (!name) name = "";

        auto setClientBufRaw = [&](void* data, uint32_t size) {
            if (inputs[i].version == QNN_TENSOR_VERSION_2) {
                inputs[i].v2.memType = QNN_TENSORMEMTYPE_RAW;
                inputs[i].v2.clientBuf.data = data;
                inputs[i].v2.clientBuf.dataSize = size;
            } else {
                inputs[i].v1.memType = QNN_TENSORMEMTYPE_RAW;
                inputs[i].v1.clientBuf.data = data;
                inputs[i].v1.clientBuf.dataSize = size;
            }
        };
        auto setIntData = [&](const int64_t* data, size_t count) {
            const size_t bytes = Impl::tensorElementCount(inputs[i]) * Impl::tensorElementSize(inputs[i]);
            inputBuffers[i].resize(bytes, 0);
            Impl::writeIntInputToTensor(data, count, inputs[i], inputBuffers[i].data());
            setClientBufRaw(inputBuffers[i].data(), static_cast<uint32_t>(bytes));
        };
        auto setFloatData = [&](const float* data, size_t count) {
            const size_t bytes = Impl::tensorElementCount(inputs[i]) * Impl::tensorElementSize(inputs[i]);
            inputBuffers[i].resize(bytes, 0);
            Impl::writeFloatInputToTensor(data, count, inputs[i], inputBuffers[i].data());
            setClientBufRaw(inputBuffers[i].data(), static_cast<uint32_t>(bytes));
        };

        if (strcmp(name, "input_ids") == 0) {
            setIntData(inputIds, 1);
        } else if (strcmp(name, "attention_mask") == 0) {
            setIntData(attentionMask, 1);
        } else if (strcmp(name, "position_scalar") == 0) {
            setIntData(positionScalar, 1);
        } else if (strcmp(name, "rope_emb") == 0) {
            setFloatData(ropeEmb.data(), ropeEmb.size());
        } else if (strcmp(name, "attention_bias") == 0) {
            setFloatData(attentionBias.data(), attentionBias.size());
        } else if (strcmp(name, "audio_features") == 0) {
            if (audioFeatures && audioFeatureSize > 0) {
                setFloatData(audioFeatures, audioFeatureSize);
            } else {
                static std::vector<float> zeroAudio(65 * 1024, 0.0f);
                setFloatData(zeroAudio.data(), zeroAudio.size());
            }
        } else if (strncmp(name, "cache_key_", 10) == 0) {
            int layer = atoi(name + 10);
            if (layer >= 0 && layer < NUM_LAYERS) {
                setFloatData(keyWindows[layer].data(), keyWindows[layer].size());
            }
        } else if (strncmp(name, "cache_value_", 12) == 0) {
            int layer = atoi(name + 12);
            if (layer >= 0 && layer < NUM_LAYERS) {
                setFloatData(valueWindows[layer].data(), valueWindows[layer].size());
            }
        } else {
            LOGW("Unknown input tensor: %s", name);
            setClientBufRaw(nullptr, 0);
        }
    }

    // Build output tensor array from graph info
    std::vector<Qnn_Tensor_t> outputs(gi->numOutputTensors);
    std::vector<std::vector<uint8_t>> outputBuffers(gi->numOutputTensors);

    for (uint32_t i = 0; i < gi->numOutputTensors; i++) {
        outputs[i] = gi->outputTensors[i];

        // Calculate buffer size from dimensions
        uint32_t rank = Impl::tensorRank(outputs[i]);
        uint32_t* dims = Impl::tensorDims(outputs[i]);
        uint64_t elemCount = 1;
        for (uint32_t d = 0; d < rank; d++) {
            elemCount *= dims[d];
        }
        const uint64_t bufSize = elemCount * Impl::tensorElementSize(outputs[i]);

        outputBuffers[i].resize(bufSize, 0);
        if (outputs[i].version == QNN_TENSOR_VERSION_2) {
            outputs[i].v2.memType = QNN_TENSORMEMTYPE_RAW;
            outputs[i].v2.clientBuf.data = outputBuffers[i].data();
            outputs[i].v2.clientBuf.dataSize = (uint32_t)bufSize;
        } else {
            outputs[i].v1.memType = QNN_TENSORMEMTYPE_RAW;
            outputs[i].v1.clientBuf.data = outputBuffers[i].data();
            outputs[i].v1.clientBuf.dataSize = (uint32_t)bufSize;
        }
    }

    // ============================================================
    // Execute graph
    // ============================================================
    Qnn_ErrorHandle_t err = m_impl->qnnInterface.graphExecute(
        m_impl->decoderGraph,
        inputs.data(), gi->numInputTensors,
        outputs.data(), gi->numOutputTensors,
        nullptr, nullptr);

    if (err != QNN_SUCCESS) {
        LOGE("graphExecute failed: %lu", (unsigned long)err);
        return false;
    }
    LOGI("graphExecute succeeded");

    // ============================================================
    // Read outputs by tensor name
    // ============================================================
    int argmax = 0;
    float logitsSum = 0.0f;
    std::vector<float> decodedTensor;

    for (uint32_t i = 0; i < gi->numOutputTensors; i++) {
        const char* name = Impl::tensorName(outputs[i]);
        if (!name) name = "";
        const size_t elemCount = Impl::tensorElementCount(outputs[i]);
        decodedTensor.assign(elemCount, 0.0f);
        Impl::readTensorToFloat(outputs[i], outputBuffers[i].data(), decodedTensor.data(), elemCount);

        if (strcmp(name, "logits") == 0) {
            // Find argmax and compute sum
            float maxVal = -1e30f;
            for (int v = 0; v < VOCAB_SIZE; v++) {
                const float value = decodedTensor[v];
                logitsSum += value;
                if (value > maxVal) {
                    maxVal = value;
                    argmax = v;
                }
            }
            // Copy to output
            std::memcpy(outputLogits, decodedTensor.data(), VOCAB_SIZE * sizeof(float));
        } else if (strncmp(name, "value_delta_", 12) == 0) {
            int layer = atoi(name + 12);
            if (layer >= 0 && layer < NUM_LAYERS) {
                if (m_impl->currentStep < MAX_DECODE_STEPS) {
                    for (int h = 0; h < NUM_HEADS; h++) {
                        for (int d = 0; d < HEAD_DIM; d++) {
                            m_impl->fullValueCache[layer][m_impl->currentStep][h][d] =
                                decodedTensor[h * HEAD_DIM + d];
                        }
                    }
                }
            }
        }
    }

    // Now update key cache properly (we need to do key and value separately)
    // Re-read key deltas and update
    for (uint32_t i = 0; i < gi->numOutputTensors; i++) {
        const char* name = Impl::tensorName(outputs[i]);
        if (!name) name = "";
        if (strncmp(name, "key_delta_", 10) == 0) {
            int layer = atoi(name + 10);
            if (layer >= 0 && layer < NUM_LAYERS && m_impl->currentStep < MAX_DECODE_STEPS) {
                const size_t elemCount = Impl::tensorElementCount(outputs[i]);
                decodedTensor.assign(elemCount, 0.0f);
                Impl::readTensorToFloat(outputs[i], outputBuffers[i].data(), decodedTensor.data(), elemCount);
                for (int h = 0; h < NUM_HEADS; h++) {
                    for (int d = 0; d < HEAD_DIM; d++) {
                        m_impl->fullKeyCache[layer][m_impl->currentStep][h][d] =
                            decodedTensor[h * HEAD_DIM + d];
                    }
                }
            }
        }
    }

    m_impl->lastArgmax = argmax;
    m_impl->lastLogitsSum = logitsSum;

    // Gate 3: compute key_delta_0 / value_delta_0 absmax for StepDiag
    float kd0AbsMax = 0.0f;
    float vd0AbsMax = 0.0f;
    for (uint32_t i = 0; i < gi->numOutputTensors; i++) {
        const char* name = Impl::tensorName(outputs[i]);
        if (!name) name = "";
        const size_t elemCount = Impl::tensorElementCount(outputs[i]);
        if (strcmp(name, "key_delta_0") == 0 && elemCount > 0) {
            std::vector<float> kd0(elemCount, 0.0f);
            Impl::readTensorToFloat(outputs[i], outputBuffers[i].data(), kd0.data(), elemCount);
            for (size_t k = 0; k < elemCount; ++k) {
                kd0AbsMax = std::max(kd0AbsMax, std::abs(kd0[k]));
            }
        }
        if (strcmp(name, "value_delta_0") == 0 && elemCount > 0) {
            std::vector<float> vd0(elemCount, 0.0f);
            Impl::readTensorToFloat(outputs[i], outputBuffers[i].data(), vd0.data(), elemCount);
            for (size_t k = 0; k < elemCount; ++k) {
                vd0AbsMax = std::max(vd0AbsMax, std::abs(vd0[k]));
            }
        }
    }

    // Gate 2: dump output on first few steps
    if (m_impl->currentStep <= 5) {
        LOGI("[DIAG-gate2-out] step=%d argmax=%d logits_sum=%.2f", m_impl->currentStep, argmax, logitsSum);
        LOGI("[DIAG-gate2-out] step=%d key_delta_0_absmax=%.6f value_delta_0_absmax=%.6f",
             m_impl->currentStep, kd0AbsMax, vd0AbsMax);
    }

    // Gate 3: record StepDiag
    const int windowStart = std::max(0, m_impl->currentStep - W);
    const int windowCount = std::min(W, m_impl->currentStep);
    m_impl->diagRecords.push_back({
        m_impl->currentStep, position, windowStart, windowCount,
        inputToken, argmax, logitsSum, kd0AbsMax, vd0AbsMax
    });

    m_impl->currentStep++;

    LOGI("Step %d complete: argmax=%d, logits_sum=%.2f, cache_nonzero=%d",
         m_impl->currentStep, argmax, logitsSum, m_impl->getCacheNonzeroCount());

    return true;
}

int Qwen3QnnBackend::getLastArgmax() const { return m_impl->lastArgmax; }
float Qwen3QnnBackend::getLastLogitsSum() const { return m_impl->lastLogitsSum; }
int Qwen3QnnBackend::getCacheNonzeroCount() const { return m_impl->getCacheNonzeroCount(); }
bool Qwen3QnnBackend::isInitialized() const { return m_initialized; }
const std::string& Qwen3QnnBackend::backendName() const { return m_backendName; }
int Qwen3QnnBackend::getAudioFeatureFrameCapacity() const {
    if (!m_impl || !m_impl->graphsInfo || m_impl->graphsCount == 0) {
        return 0;
    }
    auto* tensor = m_impl->findInputTensor("audio_features");
    if (!tensor) {
        return 0;
    }
    const uint32_t rank = Impl::tensorRank(*tensor);
    uint32_t* dims = Impl::tensorDims(*tensor);
    if (rank < 3 || !dims) {
        return 0;
    }
    return static_cast<int>(dims[1]);
}

void Qwen3QnnBackend::resetState() {
    if (m_impl) {
        m_impl->resetCache();
    }
}

bool Qwen3QnnBackend::runDecoderSmokeTest() {
    if (!m_initialized || !m_impl->decoderGraph) {
        LOGE("Smoke test: Backend not initialized");
        return false;
    }

    LOGI("=== Decoder Smoke Test Start ===");
    LOGI("3-step cumulative KV cache test (token=0,1,2, audio_features=zero)");

    // Reset cache once at start, then let KV accumulate across steps.
    // This matches the external reference script behavior where host_cache
    // is maintained across steps and prepareWindow() takes the last W positions.
    m_impl->resetCache();

    static std::vector<float> zeroAudio(65 * 1024, 0.0f);
    const int tokenIds[3] = {0, 1, 2};
    std::vector<float> logits(151936, 0.0f);

    // Reference values measured on device with position 0-indexed (Step 20).
    // token=0,1,2, audio_features=zero, cache accumulates across steps.
    struct SmokeStepExpected {
        int argmax;
        float logitsSumTolerance;
        float logitsSum;
    };
    static constexpr SmokeStepExpected kExpected[3] = {
        {0,      500.0f, -239319.69f},   // Step 1: token=0, position=0
        {660,    500.0f, -551386.00f},   // Step 2: token=1, position=1
        {12982,  500.0f, -398292.22f},   // Step 3: token=2, position=2
    };

    bool allPassed = true;
    int actualArgmax[3] = {};
    float actualLogitsSum[3] = {};
    int actualCacheNonzero[3] = {};

    for (int step = 0; step < 3; ++step) {
        LOGI("--- Smoke Step %d: token=%d ---", step + 1, tokenIds[step]);

        if (!decodeStep(tokenIds[step], zeroAudio.data(), zeroAudio.size(), logits.data())) {
            LOGE("Smoke test: decodeStep failed at step %d", step + 1);
            return false;
        }

        actualArgmax[step] = getLastArgmax();
        actualLogitsSum[step] = getLastLogitsSum();
        actualCacheNonzero[step] = getCacheNonzeroCount();

        LOGI("  actual: argmax=%d, logits_sum=%.2f, cache_nonzero=%d",
             actualArgmax[step], actualLogitsSum[step], actualCacheNonzero[step]);

        // Check argmax exact match
        if (actualArgmax[step] != kExpected[step].argmax) {
            LOGE("Smoke test: step %d argmax mismatch: actual=%d expected=%d",
                 step + 1, actualArgmax[step], kExpected[step].argmax);
            allPassed = false;
        }

        // Check logits_sum with tolerance
        const float diff = std::abs(actualLogitsSum[step] - kExpected[step].logitsSum);
        if (diff > kExpected[step].logitsSumTolerance) {
            LOGE("Smoke test: step %d logits_sum mismatch: actual=%.2f expected=%.2f diff=%.2f (tol=%.2f)",
                 step + 1, actualLogitsSum[step], kExpected[step].logitsSum,
                 diff, kExpected[step].logitsSumTolerance);
            allPassed = false;
        }

        // Check cache_nonzero is growing (cumulative KV should increase)
        if (step > 0 && actualCacheNonzero[step] <= actualCacheNonzero[step - 1]) {
            LOGE("Smoke test: step %d cache_nonzero=%d not growing (prev=%d)",
                 step + 1, actualCacheNonzero[step], actualCacheNonzero[step - 1]);
            allPassed = false;
        }
    }

    LOGI("=== Decoder Smoke Test %s ===", allPassed ? "PASSED" : "FAILED");
    LOGI("  Step 1: argmax=%d, logits_sum=%.2f, cache_nonzero=%d",
         actualArgmax[0], actualLogitsSum[0], actualCacheNonzero[0]);
    LOGI("  Step 2: argmax=%d, logits_sum=%.2f, cache_nonzero=%d",
         actualArgmax[1], actualLogitsSum[1], actualCacheNonzero[1]);
    LOGI("  Step 3: argmax=%d, logits_sum=%.2f, cache_nonzero=%d",
         actualArgmax[2], actualLogitsSum[2], actualCacheNonzero[2]);

    return allPassed;
}

bool Qwen3QnnBackend::runKvInfluenceProbe() {
    if (!m_initialized || !m_impl->decoderGraph) {
        LOGE("KV probe: Backend not initialized");
        return false;
    }

    LOGI("=== KV Influence Probe Start ===");

    auto* gi = m_impl->graphsInfo[0];
    if (!gi) {
        LOGE("KV probe: No graph info");
        return false;
    }

    // Common inputs for both runs
    int64_t inputIds[1] = {0};
    int64_t attentionMask[1] = {1};
    // position_scalar must be > 0 to unmask past KV, otherwise KV content is irrelevant
    int64_t positionScalar[1] = {1};
    std::vector<float> ropeEmb(ROPE_FREQ_DIM, 0.0f);
    for (int i = 0; i < ROPE_FREQ_DIM; ++i) {
        ropeEmb[i] = 1.0f * kRopeInvFreq[i];  // position=1
    }
    std::vector<float> attentionBias(1, 0.0f);
    static std::vector<float> zeroAudio(65 * 1024, 0.0f);

    auto runOnce = [&](const float* overrideKeyData, const float* overrideValueData, float* outLogitsSum) -> bool {
        // Build input tensors
        std::vector<std::vector<uint8_t>> inputBuffers(gi->numInputTensors);
        std::vector<Qnn_Tensor_t> inputs(gi->numInputTensors);

        for (uint32_t i = 0; i < gi->numInputTensors; i++) {
            inputs[i] = gi->inputTensors[i];
            const char* name = Impl::tensorName(inputs[i]);
            if (!name) name = "";

            auto setClientBufRaw = [&](void* data, uint32_t size) {
                if (inputs[i].version == QNN_TENSOR_VERSION_2) {
                    inputs[i].v2.memType = QNN_TENSORMEMTYPE_RAW;
                    inputs[i].v2.clientBuf.data = data;
                    inputs[i].v2.clientBuf.dataSize = size;
                } else {
                    inputs[i].v1.memType = QNN_TENSORMEMTYPE_RAW;
                    inputs[i].v1.clientBuf.data = data;
                    inputs[i].v1.clientBuf.dataSize = size;
                }
            };
            auto setIntData = [&](const int64_t* data, size_t count) {
                const size_t bytes = Impl::tensorElementCount(inputs[i]) * Impl::tensorElementSize(inputs[i]);
                inputBuffers[i].resize(bytes, 0);
                Impl::writeIntInputToTensor(data, count, inputs[i], inputBuffers[i].data());
                setClientBufRaw(inputBuffers[i].data(), static_cast<uint32_t>(bytes));
            };
            auto setFloatData = [&](const float* data, size_t count) {
                const size_t bytes = Impl::tensorElementCount(inputs[i]) * Impl::tensorElementSize(inputs[i]);
                inputBuffers[i].resize(bytes, 0);
                Impl::writeFloatInputToTensor(data, count, inputs[i], inputBuffers[i].data());
                setClientBufRaw(inputBuffers[i].data(), static_cast<uint32_t>(bytes));
            };

            if (strcmp(name, "input_ids") == 0) {
                setIntData(inputIds, 1);
            } else if (strcmp(name, "attention_mask") == 0) {
                setIntData(attentionMask, 1);
            } else if (strcmp(name, "position_scalar") == 0) {
                setIntData(positionScalar, 1);
            } else if (strcmp(name, "rope_emb") == 0) {
                setFloatData(ropeEmb.data(), ropeEmb.size());
            } else if (strcmp(name, "attention_bias") == 0) {
                setFloatData(attentionBias.data(), attentionBias.size());
            } else if (strcmp(name, "audio_features") == 0) {
                setFloatData(zeroAudio.data(), zeroAudio.size());
            } else if (strncmp(name, "cache_key_", 10) == 0) {
                int layer = atoi(name + 10);
                if (layer >= 0 && layer < NUM_LAYERS && overrideKeyData) {
                    // Use override data for this layer
                    const size_t windowElems = static_cast<size_t>(W) * NUM_HEADS * HEAD_DIM;
                    const size_t layerOffset = static_cast<size_t>(layer) * windowElems;
                    setFloatData(overrideKeyData + layerOffset, windowElems);
                } else {
                    // Zero-fill
                    std::vector<float> zeroWindow(W * NUM_HEADS * HEAD_DIM, 0.0f);
                    setFloatData(zeroWindow.data(), zeroWindow.size());
                }
            } else if (strncmp(name, "cache_value_", 12) == 0) {
                int layer = atoi(name + 12);
                if (layer >= 0 && layer < NUM_LAYERS && overrideValueData) {
                    const size_t windowElems = static_cast<size_t>(W) * NUM_HEADS * HEAD_DIM;
                    const size_t layerOffset = static_cast<size_t>(layer) * windowElems;
                    setFloatData(overrideValueData + layerOffset, windowElems);
                } else {
                    std::vector<float> zeroWindow(W * NUM_HEADS * HEAD_DIM, 0.0f);
                    setFloatData(zeroWindow.data(), zeroWindow.size());
                }
            } else {
                setClientBufRaw(nullptr, 0);
            }
        }

        // Build output tensors
        std::vector<Qnn_Tensor_t> outputs(gi->numOutputTensors);
        std::vector<std::vector<uint8_t>> outputBuffers(gi->numOutputTensors);
        for (uint32_t i = 0; i < gi->numOutputTensors; i++) {
            outputs[i] = gi->outputTensors[i];
            const size_t elemCount = Impl::tensorElementCount(outputs[i]);
            const size_t bufSize = elemCount * Impl::tensorElementSize(outputs[i]);
            outputBuffers[i].resize(bufSize, 0);
            if (outputs[i].version == QNN_TENSOR_VERSION_2) {
                outputs[i].v2.memType = QNN_TENSORMEMTYPE_RAW;
                outputs[i].v2.clientBuf.data = outputBuffers[i].data();
                outputs[i].v2.clientBuf.dataSize = static_cast<uint32_t>(bufSize);
            } else {
                outputs[i].v1.memType = QNN_TENSORMEMTYPE_RAW;
                outputs[i].v1.clientBuf.data = outputBuffers[i].data();
                outputs[i].v1.clientBuf.dataSize = static_cast<uint32_t>(bufSize);
            }
        }

        // Execute
        Qnn_ErrorHandle_t err = m_impl->qnnInterface.graphExecute(
            m_impl->decoderGraph,
            inputs.data(), gi->numInputTensors,
            outputs.data(), gi->numOutputTensors,
            nullptr, nullptr);
        if (err != QNN_SUCCESS) {
            LOGE("KV probe: graphExecute failed: %lu", static_cast<unsigned long>(err));
            return false;
        }

        // Read logits
        *outLogitsSum = 0.0f;
        for (uint32_t i = 0; i < gi->numOutputTensors; i++) {
            const char* name = Impl::tensorName(outputs[i]);
            if (name && strcmp(name, "logits") == 0) {
                const size_t elemCount = Impl::tensorElementCount(outputs[i]);
                std::vector<float> logitsData(elemCount, 0.0f);
                Impl::readTensorToFloat(outputs[i], outputBuffers[i].data(), logitsData.data(), elemCount);
                for (size_t v = 0; v < static_cast<size_t>(VOCAB_SIZE) && v < elemCount; ++v) {
                    *outLogitsSum += logitsData[v];
                }
                break;
            }
        }
        return true;
    };

    // Run A: all KV = zero
    float logitsSumA = 0.0f;
    LOGI("KV probe: Run A (zero KV)...");
    if (!runOnce(nullptr, nullptr, &logitsSumA)) {
        LOGE("KV probe: Run A failed");
        return false;
    }
    LOGI("KV probe: Run A logits_sum=%.2f", logitsSumA);

    // Run B: KV = deterministic non-zero data
    // Allocate contiguous buffer: [NUM_LAYERS][W * NUM_HEADS * HEAD_DIM]
    // cache_key quantization range is [-512, 512], cache_value range is [-1, 1].
    // Keep values within these bounds so quantization does not clip.
    const size_t windowElems = static_cast<size_t>(W) * NUM_HEADS * HEAD_DIM;
    std::vector<float> nonZeroKeyData(NUM_LAYERS * windowElems);
    std::vector<float> nonZeroValueData(NUM_LAYERS * windowElems);
    for (int layer = 0; layer < NUM_LAYERS; ++layer) {
        for (size_t idx = 0; idx < windowElems; ++idx) {
            const int h = static_cast<int>(idx / HEAD_DIM);
            const int d = static_cast<int>(idx % HEAD_DIM);
            const float phase = static_cast<float>(layer) * 0.1f +
                                static_cast<float>(h) * 0.01f +
                                static_cast<float>(d) * 0.001f;
            // key range [-512, 512]: sin ∈ [-1,1] → * 10.0 ∈ [-10, 10], well within bounds
            nonZeroKeyData[static_cast<size_t>(layer) * windowElems + idx] =
                std::sin(phase) * 10.0f;
            // value range [-1, 1]: cos ∈ [-1,1] → * 0.8 ∈ [-0.8, 0.8], within bounds
            nonZeroValueData[static_cast<size_t>(layer) * windowElems + idx] =
                std::cos(phase) * 0.8f;
        }
    }

    float logitsSumB = 0.0f;
    LOGI("KV probe: Run B (non-zero KV)...");
    if (!runOnce(nonZeroKeyData.data(), nonZeroValueData.data(), &logitsSumB)) {
        LOGE("KV probe: Run B failed");
        return false;
    }
    LOGI("KV probe: Run B logits_sum=%.2f", logitsSumB);

    const float diff = std::abs(logitsSumA - logitsSumB);
    const bool passed = diff > 1.0f;
    LOGI("=== KV Influence Probe %s ===", passed ? "PASSED" : "FAILED");
    LOGI("  Run A logits_sum=%.2f", logitsSumA);
    LOGI("  Run B logits_sum=%.2f", logitsSumB);
    LOGI("  diff=%.2f (threshold=1.0)", diff);
    return passed;
}

bool Qwen3QnnBackend::auditRuntimeEncodings() {
    if (!m_initialized || !m_impl->decoderGraph) {
        LOGE("Encoding audit: Backend not initialized");
        return false;
    }

    LOGI("=== Gate A: Runtime Encoding Audit ===");

    auto* gi = m_impl->graphsInfo[0];
    if (!gi) {
        LOGE("Encoding audit: No graph info");
        return false;
    }

    // ============================================================
    // Path 1: Read quantizeParams from stored GraphInfo tensor structs
    // ============================================================
    LOGI("--- Path 1: GraphInfo stored tensor encoding (after graphFinalize) ---");

    const char* targetNames[] = {
        "cache_key_0", "cache_key_27",
        "cache_value_0", "cache_value_27"
    };

    for (uint32_t i = 0; i < gi->numInputTensors; i++) {
        const char* name = Impl::tensorName(gi->inputTensors[i]);
        if (!name) continue;

        bool isTarget = false;
        for (const auto& tn : targetNames) {
            if (strcmp(name, tn) == 0) { isTarget = true; break; }
        }
        if (!isTarget) continue;

        Impl::logTensorEncoding("Path1", gi->inputTensors[i]);
    }

    // All input tensors summary
    LOGI("--- Path 1: All input tensors encoding summary ---");
    for (uint32_t i = 0; i < gi->numInputTensors; i++) {
        const char* name = Impl::tensorName(gi->inputTensors[i]);
        if (!name) name = "?";
        const auto& qp = Impl::tensorQuantParams(gi->inputTensors[i]);
        const auto dataType = Impl::tensorDataType(gi->inputTensors[i]);

        if (qp.encodingDefinition == QNN_DEFINITION_DEFINED &&
            qp.quantizationEncoding == QNN_QUANTIZATION_ENCODING_SCALE_OFFSET) {
            LOGI("[Path1-summary] input[%u]: %s dtype=%d scale=%.10e offset=%d encDef=DEFINED",
                 i, name, (int)dataType, qp.scaleOffsetEncoding.scale, qp.scaleOffsetEncoding.offset);
        } else if (qp.encodingDefinition == QNN_DEFINITION_IMPL_GENERATED) {
            LOGI("[Path1-summary] input[%u]: %s dtype=%d encDef=IMPL_GENERATED",
                 i, name, (int)dataType);
        } else {
            LOGI("[Path1-summary] input[%u]: %s dtype=%d encDef=%d quantEnc=%d",
                 i, name, (int)dataType, (int)qp.encodingDefinition, (int)qp.quantizationEncoding);
        }
    }

    // All output tensors summary
    LOGI("--- Path 1: All output tensors encoding summary ---");
    for (uint32_t i = 0; i < gi->numOutputTensors; i++) {
        const char* name = Impl::tensorName(gi->outputTensors[i]);
        if (!name) name = "?";
        const auto& qp = Impl::tensorQuantParams(gi->outputTensors[i]);
        const auto dataType = Impl::tensorDataType(gi->outputTensors[i]);

        if (qp.encodingDefinition == QNN_DEFINITION_DEFINED &&
            qp.quantizationEncoding == QNN_QUANTIZATION_ENCODING_SCALE_OFFSET) {
            LOGI("[Path1-summary] output[%u]: %s dtype=%d scale=%.10e offset=%d",
                 i, name, (int)dataType, qp.scaleOffsetEncoding.scale, qp.scaleOffsetEncoding.offset);
        } else {
            LOGI("[Path1-summary] output[%u]: %s dtype=%d encDef=%d quantEnc=%d",
                 i, name, (int)dataType, (int)qp.encodingDefinition, (int)qp.quantizationEncoding);
        }
    }

    // ============================================================
    // Path 2: Context binary introspection via QnnSystemContext
    // ============================================================
    LOGI("--- Path 2: Context binary introspection ---");

    bool path2Ok = false;

    if (!m_impl->qnnInterface.contextGetBinarySize) {
        LOGW("[Path2] contextGetBinarySize function pointer is null, skipping Path 2");
    } else {
        Qnn_ContextBinarySize_t binarySize = 0;
        Qnn_ErrorHandle_t err = m_impl->qnnInterface.contextGetBinarySize(
            m_impl->contextHandle, &binarySize);
        if (err != QNN_SUCCESS || binarySize == 0) {
            LOGE("[Path2] contextGetBinarySize failed: err=%lu size=%lu",
                 static_cast<unsigned long>(err), static_cast<unsigned long>(binarySize));
        } else {
            LOGI("[Path2] Context binary size: %lu bytes",
                 static_cast<unsigned long>(binarySize));

            if (!m_impl->qnnInterface.contextGetBinary) {
                LOGW("[Path2] contextGetBinary function pointer is null");
            } else {
                std::vector<uint8_t> binaryBuffer(binarySize, 0);
                Qnn_ContextBinarySize_t writtenSize = 0;
                err = m_impl->qnnInterface.contextGetBinary(
                    m_impl->contextHandle,
                    binaryBuffer.data(),
                    binarySize,
                    &writtenSize);
                if (err != QNN_SUCCESS || writtenSize == 0) {
                    LOGE("[Path2] contextGetBinary failed: err=%lu written=%lu",
                         static_cast<unsigned long>(err),
                         static_cast<unsigned long>(writtenSize));
                } else {
                    LOGI("[Path2] Context binary retrieved: %lu bytes",
                         static_cast<unsigned long>(writtenSize));

                    // Use QnnSystemContext to introspect
                    bool useNewApi = (m_impl->qnnSystemInterface.systemContextCreate != nullptr &&
                                      m_impl->qnnSystemInterface.systemContextGetMetaData != nullptr &&
                                      m_impl->qnnSystemInterface.systemContextFree != nullptr);
                    bool useOldApi = !useNewApi &&
                                     (m_impl->qnnSystemInterface.systemContextCreate != nullptr &&
                                      m_impl->qnnSystemInterface.systemContextGetBinaryInfo != nullptr &&
                                      m_impl->qnnSystemInterface.systemContextFree != nullptr);

                    if (!useNewApi && !useOldApi) {
                        LOGW("[Path2] No QnnSystemContext API available, skipping");
                    } else {
                        QnnSystemContext_Handle_t sysCtxHandle = nullptr;
                        err = m_impl->qnnSystemInterface.systemContextCreate(&sysCtxHandle);
                        if (err != QNN_SUCCESS || !sysCtxHandle) {
                            LOGE("[Path2] QnnSystemContext_create failed: %lu",
                                 static_cast<unsigned long>(err));
                        } else {
                            const QnnSystemContext_BinaryInfo_t* binaryInfo = nullptr;

                            if (useNewApi) {
                                err = m_impl->qnnSystemInterface.systemContextGetMetaData(
                                    sysCtxHandle,
                                    binaryBuffer.data(),
                                    static_cast<Qnn_ContextBinarySize_t>(writtenSize),
                                    &binaryInfo);
                            } else {
                                Qnn_ContextBinarySize_t infoSize = 0;
                                err = m_impl->qnnSystemInterface.systemContextGetBinaryInfo(
                                    sysCtxHandle,
                                    binaryBuffer.data(),
                                    static_cast<uint64_t>(writtenSize),
                                    &binaryInfo,
                                    &infoSize);
                            }

                            if (err != QNN_SUCCESS || !binaryInfo) {
                                LOGE("[Path2] %s failed: %lu",
                                     useNewApi ? "systemContextGetMetaData" : "systemContextGetBinaryInfo",
                                     static_cast<unsigned long>(err));
                            } else {
                                m_impl->logBinaryInfo(binaryInfo);
                                path2Ok = true;
                            }
                            m_impl->qnnSystemInterface.systemContextFree(sysCtxHandle);
                        }
                    }
                }
            }
        }
    }

    LOGI("=== Gate A: Runtime Encoding Audit %s ===", path2Ok ? "COMPLETE" : "PARTIAL (Path 2 skipped)");
    LOGI("Compare Path 1 (GraphInfo stored) vs Path 2 (context binary) encoding scales.");
    LOGI("If both show scale=1.53e-9 for cache_key/cache_value, HTP overrides at finalize.");
    LOGI("If Path 1 shows 0.015625 but Path 2 shows 1.53e-9, stored structs are stale.");
    return true;
}

void Qwen3QnnBackend::dumpDiagRecords() const {
    if (!m_impl || m_impl->diagRecords.empty()) return;
    LOGI("=== StepDiag Records (%zu steps) ===", m_impl->diagRecords.size());
    LOGI("  %4s %8s %8s %8s %8s %8s %12s %10s %10s",
         "step", "pos", "winSt", "winCnt", "inTok", "argmax", "logitsSum", "kd0Max", "vd0Max");
    for (const auto& r : m_impl->diagRecords) {
        LOGI("  %4d %8d %8d %8d %8d %8d %12.2f %10.4f %10.4f",
             r.step, r.position, r.windowStart, r.windowCount,
             r.inputToken, r.argmax, r.logitsSum, r.keyDelta0Max, r.valueDelta0Max);
    }
    LOGI("=== End StepDiag Records ===");
}

void Qwen3QnnBackend::release() {
    if (m_impl) {
        m_impl->resetCache();
        m_impl->release();

        // Close libraries
        if (m_impl->decoderLib) {
            dlclose(m_impl->decoderLib);
            m_impl->decoderLib = nullptr;
        }
        if (m_impl->qnnSystemLib) {
            dlclose(m_impl->qnnSystemLib);
            m_impl->qnnSystemLib = nullptr;
        }
        if (m_impl->qnnHtpLib) {
            dlclose(m_impl->qnnHtpLib);
            m_impl->qnnHtpLib = nullptr;
        }
    }
    m_initialized = false;
    LOGI("Qwen3 QNN backend released");
}

bool Qwen3QnnBackend::runConvFrontend(const float* inputFeatures, int inputFrames,
                                      std::vector<float>* output, int* outputFrames) {
    if (!m_initialized || !m_impl->convSession || !inputFeatures || inputFrames <= 0 || !output || !outputFrames) {
        return false;
    }

    try {
        std::array<int64_t, 3> inputShape{1, inputFrames, 128};
        Ort::Value inputTensor = Ort::Value::CreateTensor<float>(
            m_impl->cpuMemoryInfo,
            const_cast<float*>(inputFeatures),
            static_cast<size_t>(inputFrames) * 128,
            inputShape.data(),
            inputShape.size());

        std::array<Ort::Value, 1> inputs = {std::move(inputTensor)};
        auto outputs = m_impl->convSession->Run(
            Ort::RunOptions{nullptr},
            m_impl->convInputNamePtrs.data(),
            inputs.data(),
            inputs.size(),
            m_impl->convOutputNamePtrs.data(),
            m_impl->convOutputNamePtrs.size());
        if (outputs.empty()) {
            LOGE("Conv frontend returned empty outputs");
            return false;
        }

        auto info = outputs[0].GetTensorTypeAndShapeInfo();
        auto shape = info.GetShape();
        if (shape.size() != 3 || shape[0] != 1 || shape[2] != CONV_HIDDEN_DIM || shape[1] <= 0) {
            LOGE("Unexpected conv output shape");
            return false;
        }

        *outputFrames = static_cast<int>(shape[1]);
        const size_t count = static_cast<size_t>(*outputFrames) * CONV_HIDDEN_DIM;
        output->assign(outputs[0].GetTensorData<float>(), outputs[0].GetTensorData<float>() + count);
        LOGI("Conv frontend completed: input_frames=%d output_frames=%d", inputFrames, *outputFrames);
        return true;
    } catch (const Ort::Exception& e) {
        LOGE("Conv frontend failed: %s", e.what());
        return false;
    }
}

bool Qwen3QnnBackend::runEncoder(const float* inputFeatures, int convFrames,
                                 int validAudioTokens, std::vector<float>* output) {
    if (!m_initialized || !m_impl->encoderSession || !inputFeatures || convFrames <= 0 || !output) {
        return false;
    }

    validAudioTokens = std::clamp(validAudioTokens, 0, convFrames);
    try {
        std::array<int64_t, 3> inputShape{1, convFrames, CONV_HIDDEN_DIM};
        Ort::Value convTensor = Ort::Value::CreateTensor<float>(
            m_impl->cpuMemoryInfo,
            const_cast<float*>(inputFeatures),
            static_cast<size_t>(convFrames) * CONV_HIDDEN_DIM,
            inputShape.data(),
            inputShape.size());

        std::vector<uint8_t> mask(static_cast<size_t>(convFrames), 0);
        std::fill_n(mask.begin(), validAudioTokens, static_cast<uint8_t>(1));
        std::array<int64_t, 2> maskShape{1, convFrames};
        Ort::Value maskTensor = Ort::Value::CreateTensor<bool>(
            m_impl->cpuMemoryInfo,
            reinterpret_cast<bool*>(mask.data()),
            static_cast<size_t>(convFrames),
            maskShape.data(),
            maskShape.size());

        std::array<Ort::Value, 2> inputs = {std::move(convTensor), std::move(maskTensor)};
        auto outputs = m_impl->encoderSession->Run(
            Ort::RunOptions{nullptr},
            m_impl->encoderInputNamePtrs.data(),
            inputs.data(),
            inputs.size(),
            m_impl->encoderOutputNamePtrs.data(),
            m_impl->encoderOutputNamePtrs.size());
        if (outputs.empty()) {
            LOGE("Encoder returned empty outputs");
            return false;
        }

        auto info = outputs[0].GetTensorTypeAndShapeInfo();
        auto shape = info.GetShape();
        if (shape.size() != 3 || shape[0] != 1 || shape[2] != ENCODER_HIDDEN_DIM || shape[1] <= 0) {
            LOGE("Unexpected encoder output shape");
            return false;
        }

        const size_t count = static_cast<size_t>(shape[1]) * ENCODER_HIDDEN_DIM;
        output->assign(outputs[0].GetTensorData<float>(), outputs[0].GetTensorData<float>() + count);
        LOGI("Encoder completed: conv_frames=%d valid_audio_tokens=%d output_frames=%lld",
             convFrames, validAudioTokens, static_cast<long long>(shape[1]));

        // [DIAG-audio-features] Log encoder output range
        {
            const float* data = output->data();
            const size_t count = output->size();
            float minVal = std::numeric_limits<float>::max();
            float maxVal = std::numeric_limits<float>::lowest();
            float sum = 0.0f;
            size_t nonzero = 0;
            for (size_t i = 0; i < count; ++i) {
                minVal = std::min(minVal, data[i]);
                maxVal = std::max(maxVal, data[i]);
                sum += data[i];
                if (data[i] != 0.0f) ++nonzero;
            }
            LOGI("[DIAG-audio-features] encoder output: count=%zu, min=%.6f, max=%.6f, sum=%.2f, nonzero=%zu",
                 count, minVal, maxVal, sum, nonzero);
            LOGI("[DIAG-audio-features] first 5 values: %.6f %.6f %.6f %.6f %.6f",
                 data[0], data[1], data[2], data[3], data[4]);
        }

        return true;
    } catch (const Ort::Exception& e) {
        LOGE("Encoder failed: %s", e.what());
        return false;
    }
}

} // namespace stt
