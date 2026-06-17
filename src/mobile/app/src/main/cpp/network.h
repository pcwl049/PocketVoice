#pragma once

#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <vector>
#include <mutex>
#include <condition_variable>

namespace stt {

struct AudioData {
    std::vector<float> samples;
    uint32_t sample_rate;
    uint16_t channels;
    uint32_t segment_id = 0;
    bool is_final;
};

class NetworkServer {
public:
    using AudioCallback = std::function<void(const AudioData&)>;
    using ErrorCallback = std::function<void(const std::string&)>;
    
    NetworkServer();
    ~NetworkServer();
    
    bool start(int port);
    void stop();
    bool isRunning() const;
    
    void setAudioCallback(AudioCallback callback);
    void setErrorCallback(ErrorCallback callback);
    
    bool sendText(const std::string& text, const std::string& emotion, const std::string& event, uint32_t segmentId = 0);
    
private:
    void acceptThread();
    void clientThread(int clientSocket);
    void closeClientIfCurrent(int clientSocket);
    void markPendingFinalSegment(uint32_t segmentId);
    void notifySegmentSent(uint32_t segmentId);
    
    struct Impl;
    Impl* m_impl = nullptr;
    
    std::atomic<bool> m_running{false};
    std::thread m_acceptThread;
    std::thread m_clientThread;
    std::mutex m_clientMutex;
    
    AudioCallback m_audioCallback;
    ErrorCallback m_errorCallback;
    
    int m_port = 18080;
    int m_clientSocket = -1;
    std::mutex m_responseMutex;
    std::condition_variable m_responseCv;
    uint32_t m_pendingFinalSegmentId = 0;
    bool m_waitingFinalResponse = false;
};

}
