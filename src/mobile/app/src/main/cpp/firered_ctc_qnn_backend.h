#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace stt {

/**
 * FireRedASR2-CTC via ONNX Runtime QNN Execution Provider.
 *
 * Runs the statically-quantized QDQ uint8 FireRedASR2-CTC model on the
 * Qualcomm HTP (Hexagon Tensor Processor) through ORT's QNN EP
 * (libonnxruntime.so built with --use_qnn static_lib). This is a
 * non-autoregressive single-pass CTC model: input fbank [1, T, 80]
 * -> log_probs [1, T/4, 8667] -> greedy CTC decode (blank id = 0).
 *
 * Falls back to the CPU EP automatically if the QNN backend path cannot
 * be initialized (e.g. QNN runtime libs not bundled or DSP unavailable),
 * so the same model also runs on plain arm64 hardware.
 */
class FireRedCtcQnnBackend {
public:
    FireRedCtcQnnBackend();
    ~FireRedCtcQnnBackend();

    /**
     * Initialize the backend.
     * @param modelPath Full path to model.onnx (QDQ uint8 static).
     * @param tokensPath Full path to tokens.txt.
     * @param qnnLibDir Directory containing libQnnHtp.so (may be empty to
     *                  attempt backend_path = bare filename).
     * @param useHtp If true, try QNN HTP EP first; else CPU EP only.
     * @return true if session creation succeeded (any EP).
     */
    bool init(const std::string& modelPath, const std::string& tokensPath,
              const std::string& qnnLibDir, bool useHtp);

    /**
     * Recognize one utterance.
     * @param fbank Fbank features [frames][80], 16 kHz, 10 ms shift.
     * @param frames Number of feature frames (T).
     * @param text  Output recognized text (UTF-8, no trailing newline).
     * @return true on success (text may be empty for silence).
     */
    bool recognize(const float* fbank, int frames, std::string* text);

    bool isInitialized() const;
    const std::string& backendName() const;
    int lastDecodeMs() const;
    bool qnnActive() const;

    // Per-dim fbank CMVN from model metadata (cmvn_mean / cmvn_inv_stddev).
    // Returns false when the model carries no CMVN metadata.
    bool cmvn(const float** mean, const float** invStd, int* dim) const;

    /**
     * Release all resources (session, ort env).
     */
    void release();

private:
    struct Impl;
    Impl* m_impl = nullptr;
    bool m_initialized = false;
    std::string m_backendName = "fire_red_asr2_ctc_qnn";
};

}  // namespace stt