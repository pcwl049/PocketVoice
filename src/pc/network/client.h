#pragma once

#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <vector>

namespace stt {

struct TextResult {
    std::string text;
    std::string emotion;
    std::string event;
    uint32_t segment_id = 0;
    bool success;
};

class NetworkClient {
public:
    using TextCallback = std::function<void(const TextResult&)>;
    using ErrorCallback = std::function<void(const std::string&)>;
    
    NetworkClient();
    ~NetworkClient();
    
    bool connect(const std::string& host, int port);
    void disconnect();
    bool isConnected() const;
    
    void sendAudio(const float* samples, size_t numSamples, bool isFinal = false, uint32_t segmentId = 0);
    void setTextCallback(TextCallback callback);
    void setErrorCallback(ErrorCallback callback);
    
private:
    void receiveThread();
    bool sendBytes(const uint8_t* data, size_t len);
    
    struct Impl;
    Impl* m_impl = nullptr;
    
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_connected{false};
    std::thread m_receiveThread;
    
    TextCallback m_textCallback;
    ErrorCallback m_errorCallback;
    
    std::string m_host;
    int m_port = 18080;
};

}
