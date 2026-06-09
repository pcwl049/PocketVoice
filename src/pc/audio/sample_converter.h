#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace stt {

enum class AudioSampleType {
    Pcm16,
    Float32,
};

struct AudioFormat {
    int sample_rate = 0;
    int channels = 0;
    AudioSampleType sample_type = AudioSampleType::Float32;
};

std::vector<float> convertTo16kMonoFloat(const uint8_t* data, size_t byteCount, const AudioFormat& format);

}  // namespace stt
