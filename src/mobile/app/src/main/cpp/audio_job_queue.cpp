#include "audio_job_queue.h"

#include <utility>

namespace stt {

AudioJobQueue::AudioJobQueue(size_t capacity)
    : m_capacity(capacity) {}

bool AudioJobQueue::tryPush(AudioData audio) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_stopped || m_queue.size() >= m_capacity) {
        ++m_droppedJobs;
        return false;
    }

    m_queue.push_back(std::move(audio));
    m_cv.notify_one();
    return true;
}

bool AudioJobQueue::waitPop(AudioData& audio) {
    std::unique_lock<std::mutex> lock(m_mutex);
    m_cv.wait(lock, [&]() {
        return m_stopped || !m_queue.empty();
    });

    if (m_queue.empty()) {
        return false;
    }

    audio = std::move(m_queue.front());
    m_queue.pop_front();
    return true;
}

void AudioJobQueue::stop() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_stopped = true;
    m_cv.notify_all();
}

void AudioJobQueue::clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_queue.clear();
}

size_t AudioJobQueue::size() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_queue.size();
}

size_t AudioJobQueue::droppedJobs() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_droppedJobs;
}

}
