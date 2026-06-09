#include "pc_status_json.h"

#include <cassert>
#include <cstdio>
#include <string>

int main() {
    stt::PcRuntimeSnapshot snapshot;
    snapshot.running = true;
    snapshot.phone_connected = true;
    snapshot.osc_ready = false;
    snapshot.typing_active = true;
    snapshot.chatbox_dry_run = false;
    snapshot.listening_active = true;
    snapshot.reconnecting = true;
    snapshot.sent_audio_count = 3;
    snapshot.last_text = "hello \"PocketVoice\"\nline";
    snapshot.last_emotion = "NEUTRAL";
    snapshot.last_error = "";
    snapshot.recent_logs.push_back({1234, stt::PcLogLevel::Info, "ADB", "forward active"});
    snapshot.recent_logs.push_back({5678, stt::PcLogLevel::Error, "Network", "connect failed"});
    snapshot.audio_input.mode = "capture";
    snapshot.audio_input.selected_device_id = "device-2";
    snapshot.audio_input.selected_device_name = "Desk Mic";
    snapshot.audio_input.devices.push_back({"device-1", "Default Mic", true});
    snapshot.audio_input.devices.push_back({"device-2", "Desk Mic", false});
    snapshot.chatbox.pending_count = 2;
    snapshot.chatbox.sent_count = 5;
    snapshot.chatbox.failed_count = 1;
    snapshot.chatbox.skipped_duplicate_count = 4;
    snapshot.chatbox.sending = true;
    snapshot.chatbox.paused = true;
    snapshot.chatbox.last_sent_text = "sent";

    std::string json = stt::toStatusJson(snapshot);
    assert(json.find("\"app\":\"PocketVoice\"") != std::string::npos);
    assert(json.find("\"running\":true") != std::string::npos);
    assert(json.find("\"phone_connected\":true") != std::string::npos);
    assert(json.find("\"osc_ready\":false") != std::string::npos);
    assert(json.find("\"typing_active\":true") != std::string::npos);
    assert(json.find("\"listening_active\":true") != std::string::npos);
    assert(json.find("\"reconnecting\":true") != std::string::npos);
    assert(json.find("\"sent_audio_count\":3") != std::string::npos);
    assert(json.find("hello \\\"PocketVoice\\\"\\nline") != std::string::npos);
    assert(json.find("\"recent_logs\"") != std::string::npos);
    assert(json.find("\"timestamp_ms\":1234") != std::string::npos);
    assert(json.find("\"level\":\"info\"") != std::string::npos);
    assert(json.find("\"category\":\"ADB\"") != std::string::npos);
    assert(json.find("\"message\":\"forward active\"") != std::string::npos);
    assert(json.find("\"level\":\"error\"") != std::string::npos);
    assert(json.find("\"audio_input\"") != std::string::npos);
    assert(json.find("\"mode\":\"capture\"") != std::string::npos);
    assert(json.find("\"selected_device_id\":\"device-2\"") != std::string::npos);
    assert(json.find("\"selected_device_name\":\"Desk Mic\"") != std::string::npos);
    assert(json.find("\"id\":\"device-1\"") != std::string::npos);
    assert(json.find("\"name\":\"Default Mic\"") != std::string::npos);
    assert(json.find("\"is_default\":true") != std::string::npos);
    assert(json.find("\"pending_count\":2") != std::string::npos);
    assert(json.find("\"skipped_duplicate_count\":4") != std::string::npos);
    assert(json.find("\"paused\":true") != std::string::npos);

    puts("pc_status_json tests passed");
    return 0;
}
