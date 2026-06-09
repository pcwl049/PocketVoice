#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace stt {

class WasapiCapture {
public:
    using SamplesCallback = std::function<void(const float* samples, size_t sampleCount)>;

    struct AudioInputDevice {
        std::string id;
        std::string name;
        bool is_default = false;
    };

    enum class Mode {
        DefaultInput,
        DefaultOutputLoopback,
    };

    WasapiCapture() = default;
    ~WasapiCapture();

    WasapiCapture(const WasapiCapture&) = delete;
    WasapiCapture& operator=(const WasapiCapture&) = delete;

    bool start(SamplesCallback callback, Mode mode = Mode::DefaultInput);
    bool start(SamplesCallback callback, Mode mode, std::string deviceId);
    void stop();
    bool isRunning() const;
    std::string lastError() const;

    static std::vector<AudioInputDevice> listInputDevices();

private:
    void run(SamplesCallback callback, Mode mode, std::string deviceId);
    void setLastError(const std::string& error);

    std::atomic<bool> m_running{false};
    std::thread m_thread;
    mutable std::mutex m_mutex;
    std::string m_lastError;
};

}  // namespace stt
