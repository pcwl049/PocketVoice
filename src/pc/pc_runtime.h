#pragma once

#include "chatbox_queue.h"
#include "pc_logger.h"

#include <mutex>
#include <string>
#include <vector>

namespace stt {

struct PcAudioInputDeviceSnapshot {
    std::string id;
    std::string name;
    bool is_default = false;
};

struct PcAudioInputSnapshot {
    std::string mode = "capture";
    std::string selected_device_id;
    std::string selected_device_name = "Default recording device";
    std::vector<PcAudioInputDeviceSnapshot> devices;
};

struct PcRuntimeSnapshot {
    bool running = false;
    bool phone_connected = false;
    bool osc_ready = false;
    bool typing_active = false;
    bool chatbox_dry_run = false;
    bool listening_active = false;
    bool reconnecting = false;
    int sent_audio_count = 0;
    std::string last_text;
    std::string last_emotion;
    std::string last_error;
    std::vector<PcLogEntry> recent_logs;
    PcAudioInputSnapshot audio_input;
    ChatBoxQueueSnapshot chatbox;
};

class PcRuntime {
public:
    void setRunning(bool running);
    void setPhoneConnected(bool connected);
    void setOscReady(bool ready);
    void setTypingActive(bool active);
    void setChatBoxDryRun(bool dryRun);
    void setListeningActive(bool active);
    void setReconnecting(bool reconnecting);
    void setSentAudioCount(int count);
    void incrementSentAudioCount();
    void setLastText(std::string text, std::string emotion);
    void setLastError(std::string error);
    void clearLastError();
    void setRecentLogs(std::vector<PcLogEntry> logs);
    void setAudioInputSnapshot(PcAudioInputSnapshot audioInput);
    void setChatBoxSnapshot(ChatBoxQueueSnapshot snapshot);
    PcRuntimeSnapshot snapshot() const;

private:
    mutable std::mutex m_mutex;
    PcRuntimeSnapshot m_snapshot;
};

}
