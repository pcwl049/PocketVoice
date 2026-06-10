#include "runtime_state.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace stt {

static constexpr size_t kMaxHistory = 8;
static constexpr size_t kMaxCacheEntries = 24;

static int64_t nowMs() {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

uint64_t RuntimeState::hashAudio(const float* samples, size_t numSamples) {
    uint64_t hash = 1469598103934665603ull;
    const auto* bytes = reinterpret_cast<const unsigned char*>(samples);
    const size_t byteCount = numSamples * sizeof(float);
    for (size_t i = 0; i < byteCount; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    hash ^= static_cast<uint64_t>(numSamples);
    hash *= 1099511628211ull;
    return hash;
}

int RuntimeState::audioMs(size_t numSamples, uint32_t sampleRate) {
    if (sampleRate == 0) return 0;
    return static_cast<int>((numSamples * 1000ull) / sampleRate);
}

CachedRecognition RuntimeState::findCached(const float* samples, size_t numSamples) {
    if (!samples || numSamples == 0) return {};
    const uint64_t key = hashAudio(samples, numSamples);
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_cacheEnabled) return {};
    auto it = m_cache.find(key);
    if (it == m_cache.end()) return {};
    return {true, it->second};
}

void RuntimeState::setRecognitionCacheEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cacheEnabled = enabled;
    if (!enabled) {
        m_cache.clear();
    }
}

void RuntimeState::recordRecognition(const float* samples, size_t numSamples, uint32_t sampleRate,
                                     int recognizeMs, const std::string& text) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const int64_t updated = nowMs();
    const int durationMs = audioMs(numSamples, sampleRate);
    m_snapshot.lastText = text;
    m_snapshot.lastAudioMs = durationMs;
    m_snapshot.lastRecognizeMs = recognizeMs;
    m_snapshot.lastUpdatedMs = updated;
    m_snapshot.totalRequests += 1;

    if (m_cacheEnabled && !text.empty() && samples && numSamples > 0) {
        if (m_cache.size() >= kMaxCacheEntries) {
            m_cache.clear();
        }
        m_cache[hashAudio(samples, numSamples)] = text;
    }

    pushHistoryLocked({text, durationMs, recognizeMs, updated, false});
}

void RuntimeState::recordCacheHit(const float* samples, size_t numSamples, uint32_t sampleRate,
                                  const std::string& text) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const int64_t updated = nowMs();
    const int durationMs = audioMs(numSamples, sampleRate);
    m_snapshot.lastText = text;
    m_snapshot.lastAudioMs = durationMs;
    m_snapshot.lastRecognizeMs = 0;
    m_snapshot.lastUpdatedMs = updated;
    m_snapshot.totalRequests += 1;
    m_snapshot.cacheHits += 1;
    pushHistoryLocked({text, durationMs, 0, updated, true});
}

RuntimeSnapshot RuntimeState::snapshot() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_snapshot;
}

void RuntimeState::reset() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_snapshot = RuntimeSnapshot{};
    m_cache.clear();
    m_cacheEnabled = false;
}

void RuntimeState::pushHistoryLocked(const RecognitionHistoryItem& item) {
    m_snapshot.history.insert(m_snapshot.history.begin(), item);
    if (m_snapshot.history.size() > kMaxHistory) {
        m_snapshot.history.resize(kMaxHistory);
    }
}

}
