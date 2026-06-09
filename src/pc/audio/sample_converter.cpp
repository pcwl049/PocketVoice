#include "sample_converter.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace stt {

namespace {

constexpr int kTargetSampleRate = 16000;

static float clampFloat(float value) {
    return std::max(-1.0f, std::min(1.0f, value));
}

static std::vector<float> readMonoFloat(const uint8_t* data, size_t byteCount, const AudioFormat& format) {
    if (!data || byteCount == 0 || format.channels <= 0) return {};

    const size_t bytesPerSample = format.sample_type == AudioSampleType::Pcm16 ? sizeof(int16_t) : sizeof(float);
    const size_t frameCount = byteCount / (bytesPerSample * (size_t)format.channels);
    std::vector<float> mono;
    mono.reserve(frameCount);

    for (size_t frame = 0; frame < frameCount; ++frame) {
        float sum = 0.0f;
        for (int channel = 0; channel < format.channels; ++channel) {
            const size_t sampleIndex = frame * (size_t)format.channels + (size_t)channel;
            if (format.sample_type == AudioSampleType::Pcm16) {
                int16_t sample = 0;
                std::memcpy(&sample, data + sampleIndex * sizeof(int16_t), sizeof(sample));
                sum += (float)sample / 32768.0f;
            } else {
                float sample = 0.0f;
                std::memcpy(&sample, data + sampleIndex * sizeof(float), sizeof(sample));
                sum += sample;
            }
        }
        mono.push_back(clampFloat(sum / (float)format.channels));
    }

    return mono;
}

}  // namespace

std::vector<float> convertTo16kMonoFloat(const uint8_t* data, size_t byteCount, const AudioFormat& format) {
    if (format.sample_rate <= 0) return {};

    auto mono = readMonoFloat(data, byteCount, format);
    if (mono.empty()) return {};
    if (format.sample_rate == kTargetSampleRate) return mono;

    const double ratio = (double)format.sample_rate / (double)kTargetSampleRate;
    const size_t outCount = (size_t)std::floor((double)mono.size() / ratio);
    std::vector<float> out;
    out.reserve(outCount);

    for (size_t i = 0; i < outCount; ++i) {
        const double sourcePos = (double)i * ratio;
        const size_t index = (size_t)sourcePos;
        const double frac = sourcePos - (double)index;
        if (index + 1 >= mono.size()) {
            out.push_back(mono[index]);
        } else {
            out.push_back((float)((1.0 - frac) * mono[index] + frac * mono[index + 1]));
        }
    }

    return out;
}

}  // namespace stt
