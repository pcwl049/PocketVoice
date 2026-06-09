#pragma once

#include "network.h"

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>

namespace stt {

class AudioJobQueue {
public:
    explicit AudioJobQueue(size_t capacity);

    bool tryPush(AudioData audio);
    bool waitPop(AudioData& audio);
    void stop();
    void clear();

    size_t size() const;
    size_t droppedJobs() const;

private:
    const size_t m_capacity;
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<AudioData> m_queue;
    bool m_stopped = false;
    size_t m_droppedJobs = 0;
};

}
