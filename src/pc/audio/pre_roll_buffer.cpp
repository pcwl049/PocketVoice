#include "pre_roll_buffer.h"

#include <algorithm>

namespace stt {

PreRollBuffer::PreRollBuffer(int sampleRate) : m_sampleRate(sampleRate) {
    setCapacitySeconds(1.0f);
}

void PreRollBuffer::setCapacitySeconds(float seconds) {
    if (seconds < 0.0f) seconds = 0.0f;
    m_capacitySamples = static_cast<size_t>(seconds * m_sampleRate);
    if (m_samples.size() > m_capacitySamples) {
        m_samples.erase(m_samples.begin(), m_samples.end() - m_capacitySamples);
    }
}

void PreRollBuffer::append(const float* samples, size_t numSamples) {
    if (numSamples == 0) return;
    m_totalSamples += numSamples;

    if (m_capacitySamples == 0) {
        m_samples.clear();
        return;
    }

    m_samples.insert(m_samples.end(), samples, samples + numSamples);
    if (m_samples.size() > m_capacitySamples) {
        m_samples.erase(m_samples.begin(), m_samples.end() - m_capacitySamples);
    }
}

void PreRollBuffer::clear() {
    m_samples.clear();
    m_totalSamples = 0;
}

std::vector<float> PreRollBuffer::getRange(float startTimeSeconds, float endTimeSeconds) const {
    if (endTimeSeconds <= startTimeSeconds || m_samples.empty()) return {};

    size_t startSample = static_cast<size_t>(std::max(0.0f, startTimeSeconds) * m_sampleRate);
    size_t endSample = static_cast<size_t>(std::max(0.0f, endTimeSeconds) * m_sampleRate);
    if (endSample <= startSample) return {};

    const size_t firstStoredSample = m_totalSamples >= m_samples.size() ? m_totalSamples - m_samples.size() : 0;
    const size_t lastStoredSample = firstStoredSample + m_samples.size();

    startSample = std::max(startSample, firstStoredSample);
    endSample = std::min(endSample, lastStoredSample);
    if (endSample <= startSample) return {};

    const size_t offset = startSample - firstStoredSample;
    return std::vector<float>(m_samples.begin() + offset, m_samples.begin() + offset + (endSample - startSample));
}

float PreRollBuffer::currentTimeSeconds() const {
    return static_cast<float>(m_totalSamples) / m_sampleRate;
}

size_t PreRollBuffer::sampleCount() const {
    return m_samples.size();
}

}
