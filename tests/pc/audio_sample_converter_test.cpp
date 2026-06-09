#include "audio/sample_converter.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

static bool near(float a, float b) {
    return std::fabs(a - b) < 0.0001f;
}

int main() {
    stt::AudioFormat stereo48kPcm16;
    stereo48kPcm16.sample_rate = 48000;
    stereo48kPcm16.channels = 2;
    stereo48kPcm16.sample_type = stt::AudioSampleType::Pcm16;

    const std::vector<int16_t> pcm16{
        32767, 32767,
        0, 0,
        -32768, -32768,
        16384, 16384,
        0, 0,
        -16384, -16384,
    };

    auto converted = stt::convertTo16kMonoFloat(
        reinterpret_cast<const uint8_t*>(pcm16.data()),
        pcm16.size() * sizeof(int16_t),
        stereo48kPcm16);

    assert(converted.size() == 2);
    assert(near(converted[0], 32767.0f / 32768.0f));
    assert(near(converted[1], 16384.0f / 32768.0f));

    stt::AudioFormat mono16kFloat;
    mono16kFloat.sample_rate = 16000;
    mono16kFloat.channels = 1;
    mono16kFloat.sample_type = stt::AudioSampleType::Float32;

    const std::vector<float> floats{0.25f, -0.5f, 1.5f};
    converted = stt::convertTo16kMonoFloat(
        reinterpret_cast<const uint8_t*>(floats.data()),
        floats.size() * sizeof(float),
        mono16kFloat);

    assert(converted.size() == 3);
    assert(near(converted[0], 0.25f));
    assert(near(converted[1], -0.5f));
    assert(near(converted[2], 1.0f));

    puts("audio_sample_converter tests passed");
    return 0;
}
