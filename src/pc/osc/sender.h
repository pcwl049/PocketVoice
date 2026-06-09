#pragma once

#include <string>
#include <functional>
#include <vector>
#include <chrono>

namespace stt {

class OscSender {
public:
    OscSender();
    ~OscSender();
    
    bool init(const std::string& ip, int port);
    bool sendChatBox(const std::string& text, bool immediate = true);
    bool sendChatBox(const std::wstring& text, bool immediate = true);
    bool sendTyping(bool typing);
    void clearChatBox();
    
private:
    bool send(const std::vector<uint8_t>& data);
    
    struct Impl;
    Impl* m_impl = nullptr;
    
    std::string m_ip;
    int m_port = 9000;
    std::chrono::steady_clock::time_point m_lastSendTime;
};

}
