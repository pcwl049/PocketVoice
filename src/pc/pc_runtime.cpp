#include "pc_runtime.h"

#include <utility>

namespace stt {

void PcRuntime::setRunning(bool running) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_snapshot.running = running;
}

void PcRuntime::setPhoneConnected(bool connected) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_snapshot.phone_connected = connected;
}

void PcRuntime::setOscReady(bool ready) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_snapshot.osc_ready = ready;
}

void PcRuntime::setTypingActive(bool active) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_snapshot.typing_active = active;
}

void PcRuntime::setChatBoxDryRun(bool dryRun) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_snapshot.chatbox_dry_run = dryRun;
}

void PcRuntime::setListeningActive(bool active) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_snapshot.listening_active = active;
}

void PcRuntime::setReconnecting(bool reconnecting) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_snapshot.reconnecting = reconnecting;
}

void PcRuntime::setSentAudioCount(int count) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_snapshot.sent_audio_count = count;
}

void PcRuntime::incrementSentAudioCount() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_snapshot.sent_audio_count++;
}

void PcRuntime::setLastText(std::string text, std::string emotion) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_snapshot.last_text = std::move(text);
    m_snapshot.last_emotion = std::move(emotion);
}

void PcRuntime::setLastError(std::string error) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_snapshot.last_error = std::move(error);
}

void PcRuntime::clearLastError() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_snapshot.last_error.clear();
}

void PcRuntime::setAudioInputSnapshot(PcAudioInputSnapshot audioInput) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_snapshot.audio_input = std::move(audioInput);
}

void PcRuntime::setChatBoxSnapshot(ChatBoxQueueSnapshot snapshot) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_snapshot.chatbox = std::move(snapshot);
}

PcRuntimeSnapshot PcRuntime::snapshot() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_snapshot;
}

}
