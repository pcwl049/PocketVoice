#include "chatbox_queue.h"

#include <algorithm>

namespace stt {

ChatBoxQueue::ChatBoxQueue(SendFn sendFn, ClockFn clockFn)
    : m_sendFn(std::move(sendFn)), m_clockFn(std::move(clockFn)) {}

void ChatBoxQueue::setIntervalMs(int64_t intervalMs) {
    if (intervalMs > 0) {
        m_intervalMs = intervalMs;
        if (m_lastSendMs < -m_intervalMs) {
            m_lastSendMs = -m_intervalMs;
        }
    }
}

void ChatBoxQueue::setPaused(bool paused) {
    m_snapshot.paused = paused;
    m_snapshot.sending = !paused && !m_pending.empty();
}

void ChatBoxQueue::enqueue(const std::vector<std::string>& texts) {
    for (const auto& text : texts) {
        if (text.empty()) continue;
        if (hasSeen(text) || std::find(m_pending.begin(), m_pending.end(), text) != m_pending.end()) {
            m_snapshot.skipped_duplicate_count++;
            continue;
        }
        m_pending.push_back(text);
    }
    m_snapshot.pending_count = m_pending.size();
    m_snapshot.sending = !m_snapshot.paused && !m_pending.empty();
}

bool ChatBoxQueue::tick() {
    if (m_snapshot.paused) {
        m_snapshot.pending_count = m_pending.size();
        m_snapshot.sending = false;
        return false;
    }

    if (m_pending.empty() || !m_sendFn || !m_clockFn) {
        m_snapshot.pending_count = m_pending.size();
        m_snapshot.sending = false;
        return false;
    }

    int64_t nowMs = m_clockFn();
    if (nowMs - m_lastSendMs < m_intervalMs) {
        m_snapshot.pending_count = m_pending.size();
        m_snapshot.sending = !m_snapshot.paused && !m_pending.empty();
        return false;
    }

    std::string text = m_pending.front();
    if (!m_sendFn(text)) {
        m_snapshot.failed_count++;
        m_snapshot.last_error = "send failed";
        m_snapshot.pending_count = m_pending.size();
        m_snapshot.sending = !m_snapshot.paused && !m_pending.empty();
        return false;
    }

    m_pending.pop_front();
    remember(text);
    m_lastSendMs = nowMs;
    m_snapshot.sent_count++;
    m_snapshot.last_sent_text = text;
    m_snapshot.last_error.clear();
    m_snapshot.pending_count = m_pending.size();
    m_snapshot.sending = !m_snapshot.paused && !m_pending.empty();
    return true;
}

void ChatBoxQueue::clear() {
    m_pending.clear();
    m_snapshot.pending_count = 0;
    m_snapshot.sending = false;
    m_snapshot.last_error.clear();
}

ChatBoxQueueSnapshot ChatBoxQueue::snapshot() const {
    ChatBoxQueueSnapshot snapshot = m_snapshot;
    snapshot.pending_count = m_pending.size();
    snapshot.sending = !snapshot.paused && !m_pending.empty();
    return snapshot;
}

bool ChatBoxQueue::hasSeen(const std::string& text) const {
    return std::find(m_recent.begin(), m_recent.end(), text) != m_recent.end();
}

void ChatBoxQueue::remember(const std::string& text) {
    m_recent.push_back(text);
    while (m_recent.size() > 10) {
        m_recent.pop_front();
    }
}

}
