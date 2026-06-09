#pragma once

#include <string>
#include <vector>
#include <functional>

namespace stt {

struct SpeechSegment {
    std::vector<float> samples;
    float start_time;
    float duration;
};

class Vad {
public:
    using SpeechCallback = std::function<void(const float*, size_t)>;
    using SegmentCallback = std::function<void(const SpeechSegment&)>;
    
    Vad();
    ~Vad();
    
    bool init(const std::string& modelPath, float threshold = 0.5f);
    void setThreshold(float threshold);
    void setCallbacks(SpeechCallback speechCb, SegmentCallback segmentCb);
    
    void process(const float* samples, size_t numSamples);
    void flush();
    void reset();
    
    bool isSpeech() const;
    bool isInSpeech() const;
    
private:
    void detectSegment();
    
    struct Impl;
    Impl* m_impl = nullptr;
    
    float m_threshold = 0.5f;
    float m_minSpeechDuration = 0.3f;
    float m_maxSpeechDuration = 10.0f;
    int m_sampleRate = 16000;
    
    std::vector<float> m_buffer;
    bool m_inSpeech = false;
    float m_speechStart = 0.0f;
    float m_currentTime = 0.0f;
    float m_silenceDuration = 0.0f;
    
    SpeechCallback m_speechCallback;
    SegmentCallback m_segmentCallback;
};

}