#include "sender.h"
#include "../../common/protocol.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <chrono>

#pragma comment(lib, "ws2_32.lib")

namespace stt {

struct OscSender::Impl {
    SOCKET socket = INVALID_SOCKET;
    sockaddr_in addr = {};
};

OscSender::OscSender() : m_impl(new Impl()) {}

OscSender::~OscSender() {
    if (m_impl->socket != INVALID_SOCKET) {
        closesocket(m_impl->socket);
    }
    delete m_impl;
}

bool OscSender::init(const std::string& ip, int port) {
    m_ip = ip;
    m_port = port;
    
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        printf("[OSC] WSAStartup failed\n");
        return false;
    }
    
    m_impl->socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_impl->socket == INVALID_SOCKET) {
        printf("[OSC] Failed to create socket\n");
        return false;
    }
    
    m_impl->addr.sin_family = AF_INET;
    m_impl->addr.sin_port = htons(port);
    inet_pton(AF_INET, ip.c_str(), &m_impl->addr.sin_addr);
    
    m_lastSendTime = std::chrono::steady_clock::now() - std::chrono::milliseconds(2000);
    
    printf("[OSC] Sender initialized: %s:%d\n", ip.c_str(), port);
    return true;
}

static void padTo4(std::vector<uint8_t>& data) {
    while (data.size() % 4 != 0) {
        data.push_back(0);
    }
}

static void writeString(std::vector<uint8_t>& data, const std::string& str) {
    data.insert(data.end(), str.begin(), str.end());
    data.push_back(0);
    padTo4(data);
}

static void writeInt32(std::vector<uint8_t>& data, int32_t value) {
    data.push_back((value >> 24) & 0xFF);
    data.push_back((value >> 16) & 0xFF);
    data.push_back((value >> 8) & 0xFF);
    data.push_back(value & 0xFF);
}

bool OscSender::sendChatBox(const std::string& text, bool immediate) {
    if (m_impl->socket == INVALID_SOCKET) return false;
    
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration<float>(now - m_lastSendTime).count();
    if (elapsed < 1.5f) {
        return false;
    }
    
    std::vector<uint8_t> packet;
    
    writeString(packet, "/chatbox/input");
    
    std::string typeTag = ",s";
    typeTag += immediate ? "T" : "F";
    typeTag += "F";
    writeString(packet, typeTag);
    
    writeString(packet, text);
    
    bool result = send(packet);
    if (result) {
        m_lastSendTime = now;
    }
    
    return result;
}

bool OscSender::sendChatBox(const std::wstring& text, bool immediate) {
    if (text.empty()) return false;
    
    std::string utf8;
    utf8.reserve(text.size() * 3);
    
    for (wchar_t wc : text) {
        if (wc < 0x80) {
            utf8.push_back((char)wc);
        } else if (wc < 0x800) {
            utf8.push_back((char)(0xC0 | (wc >> 6)));
            utf8.push_back((char)(0x80 | (wc & 0x3F)));
        } else {
            utf8.push_back((char)(0xE0 | (wc >> 12)));
            utf8.push_back((char)(0x80 | ((wc >> 6) & 0x3F)));
            utf8.push_back((char)(0x80 | (wc & 0x3F)));
        }
    }
    
    return sendChatBox(utf8, immediate);
}

bool OscSender::sendTyping(bool typing) {
    if (m_impl->socket == INVALID_SOCKET) return false;

    std::vector<uint8_t> packet;
    writeString(packet, "/chatbox/typing");
    writeString(packet, typing ? ",T" : ",F");
    return send(packet);
}

void OscSender::clearChatBox() {
    sendChatBox("", true);
}

bool OscSender::send(const std::vector<uint8_t>& data) {
    if (m_impl->socket == INVALID_SOCKET) return false;
    
    int result = sendto(m_impl->socket, (const char*)data.data(), (int)data.size(), 0,
                        (sockaddr*)&m_impl->addr, sizeof(m_impl->addr));
    
    return result != SOCKET_ERROR;
}

}
