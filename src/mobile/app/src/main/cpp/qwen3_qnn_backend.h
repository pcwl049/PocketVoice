#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace stt {

/**
 * Qwen3 QNN Backend for W=4 fixed-window decoder.
 * 
 * This backend uses QNN HTP for the Qwen3 ASR decoder with
 * fixed-window KV cache (W=4) to bypass the Slice layout issue.
 */
class Qwen3QnnBackend {
public:
    Qwen3QnnBackend();
    ~Qwen3QnnBackend();
    
    /**
     * Initialize the QNN backend.
     * @param modelDir Path to qwen3-asr-0.6b-qnn directory
     * @param qnnLibDir Path to QNN runtime libraries
     * @return true if initialization succeeds
     */
    bool init(const std::string& modelDir, const std::string& qnnLibDir);
    
    /**
     * Run a single decoder step.
     * @param inputToken Current input token ID
     * @param audioFeatures Encoder output features [1, 65, 1024]
     * @param audioFeatureSize Size of audio features in floats
     * @param outputLogits Buffer for logits output [151936]
     * @return true if step succeeds
     */
    bool decodeStep(int inputToken, const float* audioFeatures, 
                    size_t audioFeatureSize, float* outputLogits);
    
    /**
     * Get the argmax of the last logits.
     */
    int getLastArgmax() const;
    
    /**
     * Get the sum of the last logits.
     */
    float getLastLogitsSum() const;
    
    /**
     * Get the cache nonzero count.
     */
    int getCacheNonzeroCount() const;
    
    /**
     * Check if the backend is initialized.
     */
    bool isInitialized() const;
    
    /**
     * Get the backend name.
     */
    const std::string& backendName() const;

    /**
     * Get decoder audio feature frame capacity from graph input shape.
     */
    int getAudioFeatureFrameCapacity() const;

    /**
     * Reset per-request KV cache and decode step state.
     */
    void resetState();
    
    /**
     * Run a 3-step decoder smoke test with reference inputs.
     * This uses the same inputs as the external qnn-net-run reference:
     *   Step 1: token=0, audio_features=zero
     *   Step 2: token=1, audio_features=zero
     *   Step 3: token=2, audio_features=zero
     * @return true if all 3 steps succeed and metrics are logged
     */
    bool runDecoderSmokeTest();

    /**
     * Gate 1: KV Influence Probe.
     * Prove that non-zero KV inputs change decoder logits.
     * Run A: all cache_key/cache_value = zero
     * Run B: cache_key/cache_value = deterministic non-zero data
     * Both runs use position_scalar > 0 (past KV unmasked).
     * @return true if logits_sum differs between Run A and Run B
     */
    bool runKvInfluenceProbe();

    /**
     * Gate A: Runtime Encoding Audit.
     * Log the actual tensor encoding HTP uses after QnnGraph_finalize
     * for cache_key/cache_value tensors.
     * Path 1: Read quantizeParams from stored GraphInfo tensor structs.
     * Path 2: Generate context binary and introspect with QnnSystemContext_getMetadata.
     * @return true if audit completed (both paths attempted)
     */
    bool auditRuntimeEncodings();

    /**
     * Gate C2: Cache Buffer Connectivity Probe.
     * Run three decoder steps with different cache inputs (zero, max-pattern,
     * realkv-like) and compare logits to determine if HTP reads cache buffers.
     * @return true if probe completed (results logged regardless)
     */
    bool runCacheBufferProbe();

    /**
     * Per-step diagnostic record for multi-step KV alignment (Gate 3).
     */
    struct StepDiag {
        int step = 0;
        int position = 0;
        int windowStart = 0;
        int windowCount = 0;
        int inputToken = 0;
        int argmax = 0;
        float logitsSum = 0.0f;
        float keyDelta0Max = 0.0f;
        float valueDelta0Max = 0.0f;
    };

    /**
     * Dump accumulated per-step diagnostic records to logcat.
     */
    void dumpDiagRecords() const;
    
    /**
     * Release resources.
     */
    void release();
    
    /**
     * Run conv frontend on audio features.
     * @param inputFeatures Input features [1, T, 128]
     * @param inputFrames Number of input feature frames T
     * @param output Output [1, Tc, 896]
     * @param outputFrames Filled with Tc on success
     * @return true if successful
     */
    bool runConvFrontend(const float* inputFeatures, int inputFrames,
                         std::vector<float>* output, int* outputFrames);
    
    /**
     * Run encoder on conv frontend output.
     * @param inputFeatures Input features [1, Tc, 896]
     * @param convFrames Number of conv frames Tc
     * @param validAudioTokens Number of valid frames in attention mask
     * @param output Output [1, Tc, 1024]
     * @return true if successful
     */
    bool runEncoder(const float* inputFeatures, int convFrames,
                    int validAudioTokens, std::vector<float>* output);

private:
    struct Impl;
    Impl* m_impl = nullptr;
    bool m_initialized = false;
    std::string m_backendName = "qwen3_asr_qnn";
};

} // namespace stt
