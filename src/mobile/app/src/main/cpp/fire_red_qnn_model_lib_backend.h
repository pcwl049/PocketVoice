// FireRedASR2-CTC via QNN model-lib (context binary) on HTP.
//
// Loads the converter-generated libmodel.so (QnnModel_composeGraphs) and runs
// the single-input (x [1, 1024, 80]) / single-output (log_probs [1, 256, 8667])
// CTC model directly on the HTP device. This bypasses the ORT QNN EP entirely,
// which cannot validate the staticized FireRed graph (Slice/Gather/Reshape on
// int8 activations are rejected by the 2.32 op package).
//
// Model contract (converted with QAIRT 2.32 + act16 + int8 weights):
//   input  x        : float32 [1, 1024, 80]   (NTF fbank, CMVN applied)
//   output log_probs: float32 [1, 256, 8667]  (4x time downsampled, CTC greedy)
// The time axis is fixed at 1024 frames (zero-padded); log_probs_len is not an
// output (the fixed-window app computes ceil(realT/4) itself).
#ifndef STT_MOBILE_FIRE_RED_QNN_MODEL_LIB_BACKEND_H_
#define STT_MOBILE_FIRE_RED_QNN_MODEL_LIB_BACKEND_H_

#include <string>
#include <vector>
#include <cstdint>

namespace stt {

class FireRedQnnModelLibBackend {
public:
    FireRedQnnModelLibBackend();
    ~FireRedQnnModelLibBackend();

    // modelDir: directory containing libmodel.so + tokens.txt.
    // qnnLibDir: directory containing libQnnHtp.so etc. (qnn-runtime).
    bool init(const std::string& modelDir, const std::string& qnnLibDir);

    // fbank: [frames x 80] float features (CMVN applied, real frame count)
    // available in `frames`; padded/truncated to the static 1024 window here.
    bool recognize(const float* fbank, int frames, std::string* text);

    bool isInitialized() const;
    const std::string& backendName() const;
    int lastDecodeMs() const;
    bool qnnActive() const;
    bool cmvn(const float** mean, const float** invStd, int* dim) const;
    void release();

private:
    struct Impl;
    Impl* m_impl;
};

}  // namespace stt

#endif  // STT_MOBILE_FIRE_RED_QNN_MODEL_LIB_BACKEND_H_