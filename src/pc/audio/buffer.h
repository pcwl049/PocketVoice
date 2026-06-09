#pragma once

#include <vector>
#include <string>
#include <chrono>

namespace stt {

struct AudioSegment {
    std::vector<float> samples;
    int64_t timestamp_ms;
    float duration;
    int segment_id;
};

class AudioBuffer {
public:
    AudioBuffer();
    
    void setMergeWindow(float seconds);
    void setMaxSegments(int max);
    void setMaxDuration(float seconds);
    void setSeparator(const std::string& sep);
    
    void addSegment(const float* samples, size_t numSamples);
    bool shouldSend() const;
    bool hasData() const;
    
    std::vector<float> getMergedAudio();
    void clear();
    
    int getSegmentCount() const;
    float getTotalDuration() const;
    float getTimeSinceLastSend() const;
    
private:
    std::vector<AudioSegment> m_segments;
    float m_mergeWindow = 1.5f;
    int m_maxSegments = 5;
    float m_maxDuration = 30.0f;
    std::string m_separator = "，";
    
    int m_nextSegmentId = 0;
    std::chrono::steady_clock::time_point m_lastSendTime;
    float m_totalDuration = 0.0f;
};

}