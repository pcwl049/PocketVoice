#include "client.h"
#include "../pc_logger.h"
#include "../../common/protocol.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#pragma comment(lib, "ws2_32.lib")

namespace stt {

struct NetworkClient::Impl {
    SOCKET socket = INVALID_SOCKET;
};

NetworkClient::NetworkClient() : m_impl(new Impl()) {}

NetworkClient::~NetworkClient() {
    disconnect();
    delete m_impl;
}

bool NetworkClient::connect(const std::string& host, int port) {
    m_host = host;
    m_port = port;
    
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        pcLog(PcLogLevel::Error, "Network", "WSAStartup failed");
        return false;
    }
    
    m_impl->socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (m_impl->socket == INVALID_SOCKET) {
        pcLog(PcLogLevel::Error, "Network", "Failed to create socket");
        return false;
    }
    
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &addr.sin_addr);
    
    pcLogf(PcLogLevel::Info, "Network", "Connecting to %s:%d", host.c_str(), port);
    
    if (::connect(m_impl->socket, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        pcLogf(PcLogLevel::Error, "Network", "Connection failed: %d", WSAGetLastError());
        closesocket(m_impl->socket);
        m_impl->socket = INVALID_SOCKET;
        return false;
    }
    
    m_connected = true;
    m_running = true;
    m_receiveThread = std::thread(&NetworkClient::receiveThread, this);
    
    pcLogf(PcLogLevel::Info, "Network", "Connected to %s:%d", host.c_str(), port);
    return true;
}

void NetworkClient::disconnect() {
    if (!m_running && !m_connected && m_impl->socket == INVALID_SOCKET) {
        return;
    }

    m_running = false;
    
    if (m_impl->socket != INVALID_SOCKET) {
        closesocket(m_impl->socket);
        m_impl->socket = INVALID_SOCKET;
    }
    
    if (m_receiveThread.joinable()) {
        m_receiveThread.join();
    }
    
    m_connected = false;
    pcLog(PcLogLevel::Info, "Network", "Disconnected");
}

bool NetworkClient::isConnected() const {
    return m_connected;
}

void NetworkClient::sendAudio(const float* samples, size_t numSamples, bool isFinal, uint32_t segmentId) {
    if (!m_connected || m_impl->socket == INVALID_SOCKET) return;
    
    AudioMessage msg;
    msg.samples.assign(samples, samples + numSamples);
    msg.segment_id = segmentId;
    msg.setFinal(isFinal);
    msg.setHasSegmentId(true);
    msg.header.length = 4 + 2 + (uint32_t)(numSamples * sizeof(float)) + 4;
    
    std::vector<uint8_t> buffer;
    buffer.reserve(8 + msg.header.length);
    
    buffer.push_back(msg.header.magic >> 8);
    buffer.push_back(msg.header.magic & 0xFF);
    buffer.push_back(msg.header.type);
    buffer.push_back(msg.header.flags);
    
    buffer.push_back((msg.header.length >> 24) & 0xFF);
    buffer.push_back((msg.header.length >> 16) & 0xFF);
    buffer.push_back((msg.header.length >> 8) & 0xFF);
    buffer.push_back(msg.header.length & 0xFF);
    
    buffer.push_back((msg.sample_rate >> 24) & 0xFF);
    buffer.push_back((msg.sample_rate >> 16) & 0xFF);
    buffer.push_back((msg.sample_rate >> 8) & 0xFF);
    buffer.push_back(msg.sample_rate & 0xFF);
    
    buffer.push_back(msg.channels >> 8);
    buffer.push_back(msg.channels & 0xFF);
    
    const uint8_t* sampleData = reinterpret_cast<const uint8_t*>(samples);
    buffer.insert(buffer.end(), sampleData, sampleData + numSamples * sizeof(float));

    buffer.push_back((msg.segment_id >> 24) & 0xFF);
    buffer.push_back((msg.segment_id >> 16) & 0xFF);
    buffer.push_back((msg.segment_id >> 8) & 0xFF);
    buffer.push_back(msg.segment_id & 0xFF);
    
    sendBytes(buffer.data(), buffer.size());
}

void NetworkClient::setTextCallback(TextCallback callback) {
    m_textCallback = callback;
}

void NetworkClient::setErrorCallback(ErrorCallback callback) {
    m_errorCallback = callback;
}

void NetworkClient::receiveThread() {
    uint8_t headerBuf[8];
    
    while (m_running && m_connected) {
        int received = recv(m_impl->socket, (char*)headerBuf, 8, MSG_WAITALL);
        if (received <= 0) {
            if (m_running) {
                pcLog(PcLogLevel::Warning, "Network", "Connection lost");
                if (m_errorCallback) {
                    m_errorCallback("Connection lost");
                }
            }
            m_connected = false;
            break;
        }
        
        MessageHeader header;
        header.magic = (headerBuf[0] << 8) | headerBuf[1];
        header.type = headerBuf[2];
        header.flags = headerBuf[3];
        header.length = (headerBuf[4] << 24) | (headerBuf[5] << 16) | (headerBuf[6] << 8) | headerBuf[7];
        
        if (header.magic != MAGIC) {
            pcLogf(PcLogLevel::Warning, "Network", "Invalid magic: 0x%04X", header.magic);
            continue;
        }
        
        if (header.type == static_cast<uint8_t>(MessageType::Text)) {
            std::vector<uint8_t> payload(header.length);
            received = recv(m_impl->socket, (char*)payload.data(), header.length, MSG_WAITALL);
            
            if (received > 0 && payload.size() >= 6) {
                TextResult result;
                result.success = true;
                
                size_t offset = 0;
                uint32_t textLen = (payload[offset] << 24) | (payload[offset+1] << 16) | 
                                   (payload[offset+2] << 8) | payload[offset+3];
                offset += 4;
                
                if (offset + textLen <= payload.size()) {
                    result.text = std::string(payload.begin() + offset, 
                                             payload.begin() + offset + textLen);
                    offset += textLen;
                }
                
                if (offset < payload.size()) {
                    result.emotion = emotionToString(static_cast<Emotion>(payload[offset]));
                }
                offset++;
                
                if (offset < payload.size()) {
                    result.event = payload[offset] == 1 ? "Speech" : "Other";
                }
                offset++;

                if ((header.flags & FLAG_HAS_SEGMENT_ID) && offset + 4 <= payload.size()) {
                    result.segment_id = (payload[offset] << 24) | (payload[offset+1] << 16) |
                                        (payload[offset+2] << 8) | payload[offset+3];
                }
                
                if (m_textCallback) {
                    m_textCallback(result);
                }
            }
        }
    }
}

bool NetworkClient::sendBytes(const uint8_t* data, size_t len) {
    if (m_impl->socket == INVALID_SOCKET) return false;
    
    int sent = send(m_impl->socket, (const char*)data, (int)len, 0);
    return sent == (int)len;
}

}
