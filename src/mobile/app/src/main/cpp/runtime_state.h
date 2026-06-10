#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <mutex>
#include <unordered_map>

namespace stt {

struct CachedRecognition {
    bool hit = false;
    std::string text;
};

struct RecognitionHistoryItem {
    std::string text;
    int audioMs = 0;
    int recognizeMs = 0;
    int64_t updatedMs = 0;
    bool cacheHit = false;
};

struct RuntimeSnapshot {
    std::string lastText;
    int lastAudioMs = 0;
    int lastRecognizeMs = 0;
    int64_t lastUpdatedMs = 0;
    int totalRequests = 0;
    int cacheHits = 0;
    std::vector<RecognitionHistoryItem> history;
};

class RuntimeState {
public:
    CachedRecognition findCached(const float* samples, size_t numSamples);
    void setRecognitionCacheEnabled(bool enabled);
    void recordRecognition(const float* samples, size_t numSamples, uint32_t sampleRate,
                           int recognizeMs, const std::string& text);
    void recordCacheHit(const float* samples, size_t numSamples, uint32_t sampleRate,
                        const std::string& text);
    RuntimeSnapshot snapshot() const;
    void reset();

private:
    static uint64_t hashAudio(const float* samples, size_t numSamples);
    static int audioMs(size_t numSamples, uint32_t sampleRate);
    void pushHistoryLocked(const RecognitionHistoryItem& item);

    mutable std::mutex m_mutex;
    RuntimeSnapshot m_snapshot;
    std::unordered_map<uint64_t, std::string> m_cache;
    bool m_cacheEnabled = false;
};

}
