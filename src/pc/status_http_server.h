#pragma once

#include "pc_runtime.h"

#include <atomic>
#include <cstdint>
#include <future>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace stt {

class StatusHttpServer {
public:
    explicit StatusHttpServer(PcRuntime& runtime);
    ~StatusHttpServer();

    using ControlFn = std::function<std::string(const std::string&, const std::string&)>;

    void setControlHandler(ControlFn handler);
    bool start(const std::string& host, int port);
    void stop();
    bool isRunning() const;

private:
    void serveLoop();
    void handleClient(uintptr_t clientSocket);
    void pruneCompletedClientTasks();
    std::string buildResponse(const std::string& method, const std::string& path, const std::string& body) const;

    PcRuntime& m_runtime;
    ControlFn m_controlFn;
    std::string m_host = "127.0.0.1";
    int m_port = 8766;
    std::atomic<bool> m_running{false};
    std::thread m_thread;
    std::mutex m_clientTasksMutex;
    std::vector<std::future<void>> m_clientTasks;
    uintptr_t m_serverSocket = static_cast<uintptr_t>(~0ull);
};

}
