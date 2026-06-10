#pragma once

#include <string>
#include <unordered_map>
#include <fstream>
#include <sstream>

namespace stt {

struct VadConfig {
    std::string model = "models/silero_vad.onnx";
    float silence_threshold = 0.45f;
    float speech_threshold = 0.45f;
    float end_silence_duration = 0.9f;
    float min_speech_duration = 0.3f;
    float max_speech_duration = 18.0f;
    int sample_rate = 16000;
};

struct MergeConfig {
    float window = 1.5f;
    int max_segments = 5;
    float max_total_duration = 30.0f;
    float inter_segment_silence = 0.15f;
    std::string separator = "，";
};

struct OscListenConfig {
    int port = 9001;
    std::string address = "/avatar/parameters/voice";
};

struct OscSendConfig {
    std::string ip = "127.0.0.1";
    int port = 9000;
    std::string chatbox_address = "/chatbox/input";
    bool send_emotion = true;
};

struct OscConfig {
    OscListenConfig listen;
    OscSendConfig send;
};

struct NetworkConfig {
    std::string mode = "usb";
    std::string host = "127.0.0.1";
    int port = 18080;
    int timeout = 5000;
    int retry_count = 3;
    int heartbeat_interval = 1000;
};

struct SttConfig {
    std::string model = "sensevoice";
    std::string language = "auto";
    bool use_emotion = true;
    bool use_event = true;
};

struct DisplayConfig {
    bool show_emotion_icon = true;
    std::unordered_map<std::string, std::string> emotion_icons = {
        {"HAPPY", "😊"},
        {"SAD", "😢"},
        {"ANGRY", "😠"},
        {"FEAR", "😨"},
        {"SURPRISE", "😲"},
        {"DISGUST", "🤢"},
        {"NEUTRAL", ""},
        {"UNKNOWN", ""}
    };
    int max_text_length = 144;
};

struct LogConfig {
    std::string level = "info";
    std::string file = "stt.log";
    int max_size = 10485760;
};

struct Config {
    std::string version = "1.0.0";
    VadConfig vad;
    MergeConfig merge;
    OscConfig osc;
    NetworkConfig network;
    SttConfig stt;
    DisplayConfig display;
    LogConfig log;
};

inline std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

inline std::string loadFile(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) return "";
    std::stringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

inline Config loadConfig(const std::string& path) {
    Config cfg;
    std::string content = loadFile(path);
    if (content.empty()) return cfg;
    
    // Find value within a section scope to avoid key collision
    auto parseInSection = [](const std::string& c, const std::string& section, const std::string& key) -> std::string {
        std::string secKey = std::string("\"") + section + "\"";
        std::string valKey = std::string("\"") + key + "\"";
        size_t secPos = c.find(secKey);
        if (secPos == std::string::npos) return "";
        size_t valPos = c.find(valKey, secPos);
        if (valPos == std::string::npos) return "";
        size_t colon = c.find(':', valPos);
        if (colon == std::string::npos) return "";
        size_t start = c.find_first_not_of(" \t\r\n", colon + 1);
        if (start == std::string::npos) return "";
        size_t end = c.find_first_of(",\n\r}", start);
        if (end == std::string::npos) end = c.length();
        std::string v = c.substr(start, end - start);
        // trim
        if (!v.empty() && v.front() == '"') v = v.substr(1);
        if (!v.empty() && v.back() == '"') v = v.substr(0, v.length() - 1);
        return v;
    };
    
    auto parseInSectionFloat = [&](const std::string& sec, const std::string& k, float def) {
        std::string v = parseInSection(content, sec, k);
        return v.empty() ? def : std::stof(v);
    };
    auto parseInSectionInt = [&](const std::string& sec, const std::string& k, int def) {
        std::string v = parseInSection(content, sec, k);
        return v.empty() ? def : std::stoi(v);
    };
    auto parseInSectionBool = [&](const std::string& sec, const std::string& k, bool def) {
        std::string v = parseInSection(content, sec, k);
        if (v == "true") return true;
        if (v == "false") return false;
        return def;
    };
    auto parseInSectionString = [&](const std::string& sec, const std::string& k, const std::string& def) {
        std::string v = parseInSection(content, sec, k);
        return v.empty() ? def : v;
    };
    
    // Top-level values
    cfg.vad.silence_threshold = parseInSectionFloat("vad", "silence_threshold", cfg.vad.silence_threshold);
    cfg.vad.speech_threshold = parseInSectionFloat("vad", "speech_threshold", cfg.vad.silence_threshold);
    cfg.vad.end_silence_duration = parseInSectionFloat("vad", "end_silence_duration", cfg.vad.end_silence_duration);
    cfg.vad.min_speech_duration = parseInSectionFloat("vad", "min_speech_duration", cfg.vad.min_speech_duration);
    cfg.vad.max_speech_duration = parseInSectionFloat("vad", "max_speech_duration", cfg.vad.max_speech_duration);
    
    cfg.merge.window = parseInSectionFloat("merge", "window", cfg.merge.window);
    cfg.merge.max_segments = parseInSectionInt("merge", "max_segments", cfg.merge.max_segments);
    cfg.merge.max_total_duration = parseInSectionFloat("merge", "max_total_duration", cfg.merge.max_total_duration);
    cfg.merge.inter_segment_silence = parseInSectionFloat("merge", "inter_segment_silence", cfg.merge.inter_segment_silence);
    cfg.merge.separator = parseInSectionString("merge", "separator", cfg.merge.separator);
    
    cfg.osc.listen.port = parseInSectionInt("listen", "port", cfg.osc.listen.port);
    cfg.osc.send.port = parseInSectionInt("send", "port", cfg.osc.send.port);
    cfg.osc.send.ip = parseInSectionString("send", "ip", cfg.osc.send.ip);
    cfg.osc.send.send_emotion = parseInSectionBool("send", "send_emotion", cfg.osc.send.send_emotion);
    
    cfg.network.mode = parseInSectionString("network", "mode", cfg.network.mode);
    cfg.network.host = parseInSectionString("network", "host", cfg.network.host);
    cfg.network.port = parseInSectionInt("network", "port", cfg.network.port);
    
    cfg.display.max_text_length = parseInSectionInt("display", "max_text_length", cfg.display.max_text_length);
    cfg.display.show_emotion_icon = parseInSectionBool("display", "show_emotion_icon", cfg.display.show_emotion_icon);
    
    return cfg;
}

}
