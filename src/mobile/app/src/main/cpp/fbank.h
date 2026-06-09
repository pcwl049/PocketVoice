#pragma once

#include <cstdint>
#include <vector>

namespace stt {

struct FbankConfig {
    int32_t sample_rate = 16000;
    int32_t num_mel_bins = 80;
    float frame_length_ms = 25.0f;
    float frame_shift_ms = 10.0f;
    float low_freq = 20.0f;
    float high_freq = 7800.0f;
    float dither = 0.0f;
    bool snip_edges = true;
};

class Fbank {
public:
    Fbank(const FbankConfig& config = FbankConfig{});
    ~Fbank();

    // Compute fbank features from raw PCM audio
    // input: float samples [-1.0, 1.0], 16kHz
    // returns: [num_frames * num_mel_bins] row-major
    std::vector<float> compute(const float* samples, size_t num_samples);

    int32_t num_frames() const { return num_mel_bins_; }

private:
    void build_mel_filterbank();
    void fft(const float* in, float* out_re, float* out_im, int n);

    FbankConfig config_;
    int32_t window_size_;
    int32_t frame_shift_;
    int32_t fft_size_;
    int32_t num_mel_bins_;
    std::vector<float> hamming_window_;
    std::vector<std::vector<float>> mel_weights_;
};

} // namespace stt
