#include "network.h"
#include "protocol.h"
#include <android/log.h>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <mutex>

#define LOG_TAG "STT_Network"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace stt {

struct NetworkServer::Impl {
    int serverSocket = -1;
};

NetworkServer::NetworkServer() : m_impl(new Impl()) {}

NetworkServer::~NetworkServer() {
    stop();
    delete m_impl;
}

bool NetworkServer::start(int port) {
    m_port = port;
    
    m_impl->serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (m_impl->serverSocket < 0) {
        LOGE("Failed to create socket");
        return false;
    }
    
    int opt = 1;
    setsockopt(m_impl->serverSocket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    
    if (bind(m_impl->serverSocket, (sockaddr*)&addr, sizeof(addr)) < 0) {
        LOGE("Failed to bind port %d", port);
        close(m_impl->serverSocket);
        return false;
    }
    
    if (listen(m_impl->serverSocket, 1) < 0) {
        LOGE("Failed to listen");
        close(m_impl->serverSocket);
        return false;
    }
    
    m_running = true;
    m_acceptThread = std::thread(&NetworkServer::acceptThread, this);
    
    LOGI("Server started on port %d", port);
    return true;
}

void NetworkServer::stop() {
    m_running = false;

    if (m_impl->serverSocket >= 0) {
        shutdown(m_impl->serverSocket, SHUT_RDWR);
        close(m_impl->serverSocket);
        m_impl->serverSocket = -1;
    }

    int clientSocket = -1;
    {
        std::lock_guard<std::mutex> lock(m_clientMutex);
        if (m_clientSocket >= 0) {
            clientSocket = m_clientSocket;
            m_clientSocket = -1;
        }
    }
    if (clientSocket >= 0) {
        shutdown(clientSocket, SHUT_RDWR);
        close(clientSocket);
    }

    if (m_clientThread.joinable()) {
        m_clientThread.join();
    }
    
    if (m_acceptThread.joinable()) {
        m_acceptThread.join();
    }
    
    LOGI("Server stopped");
}

bool NetworkServer::isRunning() const {
    return m_running;
}

void NetworkServer::setAudioCallback(AudioCallback callback) {
    m_audioCallback = callback;
}

void NetworkServer::setErrorCallback(ErrorCallback callback) {
    m_errorCallback = callback;
}

void NetworkServer::acceptThread() {
    while (m_running) {
        LOGI("Waiting for connection");
        
        sockaddr_in clientAddr = {};
        socklen_t clientLen = sizeof(clientAddr);
        
        int clientSocket = accept(m_impl->serverSocket, (sockaddr*)&clientAddr, &clientLen);
        
        if (clientSocket < 0) {
            if (m_running) {
                LOGE("Accept failed");
            }
            continue;
        }
        
        char clientIp[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &clientAddr.sin_addr, clientIp, INET_ADDRSTRLEN);
        LOGI("Client connected: %s:%d", clientIp, ntohs(clientAddr.sin_port));
        
        {
            std::lock_guard<std::mutex> lock(m_clientMutex);
            if (m_clientSocket >= 0) {
                shutdown(m_clientSocket, SHUT_RDWR);
                close(m_clientSocket);
                m_clientSocket = -1;
            }
        }

        if (m_clientThread.joinable()) {
            m_clientThread.join();
        }

        {
            std::lock_guard<std::mutex> lock(m_clientMutex);
            m_clientSocket = clientSocket;
        }

        m_clientThread = std::thread(&NetworkServer::clientThread, this, clientSocket);
    }
}

void NetworkServer::clientThread(int clientSocket) {
    uint8_t headerBuf[8];
    
    while (m_running && clientSocket >= 0) {
        int received = recv(clientSocket, (char*)headerBuf, 8, MSG_WAITALL);
        if (received <= 0) {
            bool shouldWaitForResponse = false;
            uint32_t pendingSegmentId = 0;
            {
                std::lock_guard<std::mutex> lock(m_responseMutex);
                shouldWaitForResponse = m_waitingFinalResponse;
                pendingSegmentId = m_pendingFinalSegmentId;
            }
            if (shouldWaitForResponse) {
                LOGI("Client write side closed; waiting for final response, segment=%u", pendingSegmentId);
                std::unique_lock<std::mutex> waitLock(m_responseMutex);
                m_responseCv.wait_for(waitLock, std::chrono::seconds(15), [&]() {
                    return !m_waitingFinalResponse || !m_running;
                });
            } else {
                LOGI("Client disconnected");
            }
            break;
        }
        
        MessageHeader header;
        header.magic = (headerBuf[0] << 8) | headerBuf[1];
        header.type = headerBuf[2];
        header.flags = headerBuf[3];
        header.length = (headerBuf[4] << 24) | (headerBuf[5] << 16) | 
                        (headerBuf[6] << 8) | headerBuf[7];
        
        if (header.magic != MAGIC) {
            LOGE("Invalid magic: 0x%04X", header.magic);
            continue;
        }
        
        if (header.type == static_cast<uint8_t>(MessageType::Audio)) {
            std::vector<uint8_t> payload(header.length);
            received = recv(clientSocket, (char*)payload.data(), header.length, MSG_WAITALL);
            
            if (received > 0 && payload.size() >= 6) {
                AudioData audio;
                
                size_t offset = 0;
                audio.sample_rate = (payload[offset] << 24) | (payload[offset+1] << 16) |
                                    (payload[offset+2] << 8) | payload[offset+3];
                offset += 4;
                
                audio.channels = (payload[offset] << 8) | payload[offset+1];
                offset += 2;
                
                size_t sampleBytes = payload.size() - offset;
                if (header.flags & FLAG_HAS_SEGMENT_ID) {
                    if (sampleBytes < 4) {
                        LOGE("Audio payload missing segment id");
                        continue;
                    }
                    sampleBytes -= 4;
                    const size_t idOffset = offset + sampleBytes;
                    audio.segment_id = (payload[idOffset] << 24) | (payload[idOffset+1] << 16) |
                                       (payload[idOffset+2] << 8) | payload[idOffset+3];
                }

                size_t numSamples = sampleBytes / sizeof(float);
                audio.samples.resize(numSamples);
                memcpy(audio.samples.data(), payload.data() + offset, numSamples * sizeof(float));
                
                audio.is_final = header.flags & FLAG_FINAL;
                if (audio.is_final) {
                    markPendingFinalSegment(audio.segment_id);
                }
                
                if (m_audioCallback) {
                    m_audioCallback(audio);
                }
            }
        }
    }
    
    closeClientIfCurrent(clientSocket);
}

bool NetworkServer::sendText(const std::string& text, const std::string& emotion, const std::string& event, uint32_t segmentId) {
    std::lock_guard<std::mutex> lock(m_clientMutex);
    if (m_clientSocket < 0) return false;
    
    std::vector<uint8_t> buffer;
    
    uint32_t textLen = text.size();
    buffer.push_back((textLen >> 24) & 0xFF);
    buffer.push_back((textLen >> 16) & 0xFF);
    buffer.push_back((textLen >> 8) & 0xFF);
    buffer.push_back(textLen & 0xFF);
    
    buffer.insert(buffer.end(), text.begin(), text.end());
    
    uint8_t emotionCode = 0;
    if (emotion == "HAPPY" || emotion == "<|HAPPY|>") emotionCode = 1;
    else if (emotion == "SAD" || emotion == "<|SAD|>") emotionCode = 2;
    else if (emotion == "ANGRY" || emotion == "<|ANGRY|>") emotionCode = 3;
    else if (emotion == "FEAR" || emotion == "<|FEAR|>") emotionCode = 4;
    else if (emotion == "SURPRISE" || emotion == "<|SURPRISE|>") emotionCode = 5;
    else if (emotion == "DISGUST" || emotion == "<|DISGUST|>") emotionCode = 6;
    else if (emotion == "NEUTRAL" || emotion == "<|NEUTRAL|>") emotionCode = 7;
    buffer.push_back(emotionCode);
    
    uint8_t eventCode = 0;
    if (event == "Speech" || event == "<|Speech|>") eventCode = 1;
    else if (event == "Music" || event == "<|Music|>") eventCode = 2;
    else if (event == "Noise" || event == "<|Noise|>") eventCode = 3;
    buffer.push_back(eventCode);

    buffer.push_back((segmentId >> 24) & 0xFF);
    buffer.push_back((segmentId >> 16) & 0xFF);
    buffer.push_back((segmentId >> 8) & 0xFF);
    buffer.push_back(segmentId & 0xFF);
    
    MessageHeader header;
    header.magic = MAGIC;
    header.type = static_cast<uint8_t>(MessageType::Text);
    header.flags = 0;
    if (!emotion.empty()) header.flags |= 0x01;
    if (!event.empty()) header.flags |= 0x02;
    header.flags |= FLAG_HAS_SEGMENT_ID;
    header.length = buffer.size();
    
    std::vector<uint8_t> packet;
    packet.push_back(header.magic >> 8);
    packet.push_back(header.magic & 0xFF);
    packet.push_back(header.type);
    packet.push_back(header.flags);
    packet.push_back((header.length >> 24) & 0xFF);
    packet.push_back((header.length >> 16) & 0xFF);
    packet.push_back((header.length >> 8) & 0xFF);
    packet.push_back(header.length & 0xFF);
    
    packet.insert(packet.end(), buffer.begin(), buffer.end());
    
    int sent = send(m_clientSocket, (const char*)packet.data(), packet.size(), 0);
    if (sent == (int)packet.size()) {
        notifySegmentSent(segmentId);
    }
    return sent == (int)packet.size();
}

void NetworkServer::closeClientIfCurrent(int clientSocket) {
    if (clientSocket < 0) return;

    std::lock_guard<std::mutex> lock(m_clientMutex);
    if (m_clientSocket == clientSocket) {
        shutdown(m_clientSocket, SHUT_RDWR);
        close(m_clientSocket);
        m_clientSocket = -1;
    }
}

void NetworkServer::markPendingFinalSegment(uint32_t segmentId) {
    std::lock_guard<std::mutex> lock(m_responseMutex);
    m_pendingFinalSegmentId = segmentId;
    m_waitingFinalResponse = true;
}

void NetworkServer::notifySegmentSent(uint32_t segmentId) {
    std::lock_guard<std::mutex> lock(m_responseMutex);
    if (m_waitingFinalResponse && segmentId == m_pendingFinalSegmentId) {
        m_waitingFinalResponse = false;
        m_pendingFinalSegmentId = 0;
        m_responseCv.notify_all();
    }
}

}
