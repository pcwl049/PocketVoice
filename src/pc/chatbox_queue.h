#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace stt {

struct ChatBoxQueueSnapshot {
    size_t pending_count = 0;
    size_t sent_count = 0;
    size_t failed_count = 0;
    size_t skipped_duplicate_count = 0;
    bool sending = false;
    bool paused = false;
    std::string last_sent_text;
    std::string last_error;
};

class ChatBoxQueue {
public:
    using SendFn = std::function<bool(const std::string&)>;
    using ClockFn = std::function<int64_t()>;

    ChatBoxQueue(SendFn sendFn, ClockFn clockFn);

    void setIntervalMs(int64_t intervalMs);
    void setAutoClearDelayMs(int64_t delayMs);
    void setPaused(bool paused);
    void enqueue(const std::vector<std::string>& texts);
    bool tick();
    void clear();
    ChatBoxQueueSnapshot snapshot() const;

private:
    bool hasSeen(const std::string& text) const;
    void remember(const std::string& text);

    SendFn m_sendFn;
    ClockFn m_clockFn;
    int64_t m_intervalMs = 1500;
    int64_t m_autoClearDelayMs = 0;
    int64_t m_lastSendMs = -1500;
    int64_t m_clearDueMs = -1;
    std::deque<std::string> m_pending;
    std::deque<std::string> m_recent;
    ChatBoxQueueSnapshot m_snapshot;
};

}
