#include "pc_status_json.h"

#include <sstream>

namespace stt {

static std::string jsonEscape(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (unsigned char ch : value) {
        switch (ch) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (ch < 0x20) {
                    out += ' ';
                } else {
                    out += static_cast<char>(ch);
                }
                break;
        }
    }
    return out;
}

static const char* boolText(bool value) {
    return value ? "true" : "false";
}

std::string toStatusJson(const PcRuntimeSnapshot& snapshot) {
    std::ostringstream out;
    out << "{";
    out << "\"app\":\"PocketVoice\",";
    out << "\"running\":" << boolText(snapshot.running) << ",";
    out << "\"phone_connected\":" << boolText(snapshot.phone_connected) << ",";
    out << "\"osc_ready\":" << boolText(snapshot.osc_ready) << ",";
    out << "\"typing_active\":" << boolText(snapshot.typing_active) << ",";
    out << "\"chatbox_dry_run\":" << boolText(snapshot.chatbox_dry_run) << ",";
    out << "\"listening_active\":" << boolText(snapshot.listening_active) << ",";
    out << "\"reconnecting\":" << boolText(snapshot.reconnecting) << ",";
    out << "\"sent_audio_count\":" << snapshot.sent_audio_count << ",";
    out << "\"last_text\":\"" << jsonEscape(snapshot.last_text) << "\",";
    out << "\"last_emotion\":\"" << jsonEscape(snapshot.last_emotion) << "\",";
    out << "\"last_error\":\"" << jsonEscape(snapshot.last_error) << "\",";
    out << "\"audio_input\":{";
    out << "\"mode\":\"" << jsonEscape(snapshot.audio_input.mode) << "\",";
    out << "\"selected_device_id\":\"" << jsonEscape(snapshot.audio_input.selected_device_id) << "\",";
    out << "\"selected_device_name\":\"" << jsonEscape(snapshot.audio_input.selected_device_name) << "\",";
    out << "\"devices\":[";
    for (size_t i = 0; i < snapshot.audio_input.devices.size(); ++i) {
        const auto& device = snapshot.audio_input.devices[i];
        if (i > 0) out << ",";
        out << "{";
        out << "\"id\":\"" << jsonEscape(device.id) << "\",";
        out << "\"name\":\"" << jsonEscape(device.name) << "\",";
        out << "\"is_default\":" << boolText(device.is_default);
        out << "}";
    }
    out << "]";
    out << "},";
    out << "\"chatbox\":{";
    out << "\"pending_count\":" << snapshot.chatbox.pending_count << ",";
    out << "\"sent_count\":" << snapshot.chatbox.sent_count << ",";
    out << "\"failed_count\":" << snapshot.chatbox.failed_count << ",";
    out << "\"skipped_duplicate_count\":" << snapshot.chatbox.skipped_duplicate_count << ",";
    out << "\"sending\":" << boolText(snapshot.chatbox.sending) << ",";
    out << "\"paused\":" << boolText(snapshot.chatbox.paused) << ",";
    out << "\"last_sent_text\":\"" << jsonEscape(snapshot.chatbox.last_sent_text) << "\",";
    out << "\"last_error\":\"" << jsonEscape(snapshot.chatbox.last_error) << "\"";
    out << "}";
    out << "}";
    return out.str();
}

}
