#include "buffer.h"
#include <algorithm>

namespace stt {

AudioBuffer::AudioBuffer() {
    m_lastSendTime = std::chrono::steady_clock::now();
}

void AudioBuffer::setMergeWindow(float seconds) {
    m_mergeWindow = seconds;
}

void AudioBuffer::setMaxSegments(int max) {
    m_maxSegments = max;
}

void AudioBuffer::setMaxDuration(float seconds) {
    m_maxDuration = seconds;
}

void AudioBuffer::setSeparator(const std::string& sep) {
    m_separator = sep;
}

void AudioBuffer::addSegment(const float* samples, size_t numSamples) {
    AudioSegment seg;
    seg.samples.assign(samples, samples + numSamples);
    seg.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
    seg.duration = (float)numSamples / 16000.0f;
    seg.segment_id = m_nextSegmentId++;
    
    m_segments.push_back(std::move(seg));
    m_totalDuration += seg.duration;
}

bool AudioBuffer::shouldSend() const {
    if (m_segments.empty()) return false;
    
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration<float>(now - m_lastSendTime).count();
    
    if (elapsed >= m_mergeWindow) return true;
    if ((int)m_segments.size() >= m_maxSegments) return true;
    if (m_totalDuration >= m_maxDuration) return true;
    
    return false;
}

bool AudioBuffer::hasData() const {
    return !m_segments.empty();
}

std::vector<float> AudioBuffer::getMergedAudio() {
    std::vector<float> merged;
    
    size_t totalSamples = 0;
    for (const auto& seg : m_segments) {
        totalSamples += seg.samples.size();
    }
    
    merged.reserve(totalSamples);
    
    for (const auto& seg : m_segments) {
        merged.insert(merged.end(), seg.samples.begin(), seg.samples.end());
    }
    
    m_lastSendTime = std::chrono::steady_clock::now();
    
    return merged;
}

void AudioBuffer::clear() {
    m_segments.clear();
    m_totalDuration = 0.0f;
}

int AudioBuffer::getSegmentCount() const {
    return (int)m_segments.size();
}

float AudioBuffer::getTotalDuration() const {
    return m_totalDuration;
}

float AudioBuffer::getTimeSinceLastSend() const {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration<float>(now - m_lastSendTime).count();
}

}