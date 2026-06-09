#pragma once

#include <cstddef>
#include <vector>

namespace stt {

class PreRollBuffer {
public:
    explicit PreRollBuffer(int sampleRate = 16000);

    void setCapacitySeconds(float seconds);
    void append(const float* samples, size_t numSamples);
    void clear();

    std::vector<float> getRange(float startTimeSeconds, float endTimeSeconds) const;
    float currentTimeSeconds() const;
    size_t sampleCount() const;

private:
    int m_sampleRate = 16000;
    size_t m_capacitySamples = 0;
    size_t m_totalSamples = 0;
    std::vector<float> m_samples;
};

}
