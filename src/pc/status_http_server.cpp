#include "status_http_server.h"

#include "pc_status_page.h"
#include "pc_status_json.h"
#include "pc_logger.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include <chrono>
#include <cstdlib>
#include <future>
#include <sstream>
#include <thread>
#include <utility>

#pragma comment(lib, "ws2_32.lib")

namespace stt {

static constexpr SOCKET kInvalidSocket = INVALID_SOCKET;

StatusHttpServer::StatusHttpServer(PcRuntime& runtime) : m_runtime(runtime) {}

StatusHttpServer::~StatusHttpServer() {
    stop();
}

void StatusHttpServer::setControlHandler(ControlFn handler) {
    m_controlFn = std::move(handler);
}

bool StatusHttpServer::start(const std::string& host, int port) {
    if (m_running) return true;

    m_host = host;
    m_port = port;

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        pcLog(PcLogLevel::Error, "Status", "WSAStartup failed");
        return false;
    }

    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == kInvalidSocket) {
        pcLog(PcLogLevel::Error, "Status", "Failed to create socket");
        return false;
    }

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(port));
    inet_pton(AF_INET, host.c_str(), &addr.sin_addr);

    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));

    if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        pcLogf(PcLogLevel::Error, "Status", "Failed to bind %s:%d error=%d", host.c_str(), port, WSAGetLastError());
        closesocket(sock);
        return false;
    }

    if (listen(sock, SOMAXCONN) == SOCKET_ERROR) {
        pcLogf(PcLogLevel::Error, "Status", "Failed to listen error=%d", WSAGetLastError());
        closesocket(sock);
        return false;
    }

    m_serverSocket = static_cast<uintptr_t>(sock);
    m_running = true;
    m_thread = std::thread(&StatusHttpServer::serveLoop, this);
    pcLogf(PcLogLevel::Info, "Status", "HTTP server listening at http://%s:%d", host.c_str(), port);
    return true;
}

void StatusHttpServer::stop() {
    m_running = false;
    SOCKET sock = static_cast<SOCKET>(m_serverSocket);
    if (sock != kInvalidSocket) {
        shutdown(sock, SD_BOTH);
        closesocket(sock);
        m_serverSocket = static_cast<uintptr_t>(kInvalidSocket);
    }
    if (m_thread.joinable()) {
        m_thread.join();
    }
    std::vector<std::future<void>> clientTasks;
    {
        std::lock_guard<std::mutex> lock(m_clientTasksMutex);
        clientTasks.swap(m_clientTasks);
    }
    for (auto& task : clientTasks) {
        if (task.valid()) {
            task.wait();
        }
    }
}

bool StatusHttpServer::isRunning() const {
    return m_running;
}

void StatusHttpServer::serveLoop() {
    SOCKET sock = static_cast<SOCKET>(m_serverSocket);
    while (m_running) {
        SOCKET client = accept(sock, nullptr, nullptr);
        if (client == kInvalidSocket) {
            if (m_running) {
                pcLogf(PcLogLevel::Warning, "Status", "accept failed error=%d", WSAGetLastError());
            }
            continue;
        }
        pruneCompletedClientTasks();
        std::lock_guard<std::mutex> lock(m_clientTasksMutex);
        m_clientTasks.emplace_back(std::async(std::launch::async, &StatusHttpServer::handleClient, this, static_cast<uintptr_t>(client)));
    }
}

void StatusHttpServer::pruneCompletedClientTasks() {
    using namespace std::chrono_literals;

    std::lock_guard<std::mutex> lock(m_clientTasksMutex);
    for (auto it = m_clientTasks.begin(); it != m_clientTasks.end();) {
        if (it->valid() && it->wait_for(0ms) == std::future_status::ready) {
            it->wait();
            it = m_clientTasks.erase(it);
        } else {
            ++it;
        }
    }
}

void StatusHttpServer::handleClient(uintptr_t clientSocket) {
    SOCKET client = static_cast<SOCKET>(clientSocket);
    DWORD timeoutMs = 3000;
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));

    char request[8192] = {};
    int received = recv(client, request, sizeof(request) - 1, 0);
    if (received <= 0) {
        closesocket(client);
        return;
    }

    std::string method = "GET";
    std::string path = "/";
    std::string requestText(request, request + received);
    size_t methodEnd = requestText.find(' ');
    if (methodEnd != std::string::npos) {
        method = requestText.substr(0, methodEnd);
        size_t pathStart = methodEnd + 1;
        size_t pathEnd = requestText.find(' ', pathStart);
        if (pathEnd != std::string::npos && pathEnd > pathStart) {
            path = requestText.substr(pathStart, pathEnd - pathStart);
        }
    }

    std::string body;
    size_t bodyStart = requestText.find("\r\n\r\n");
    if (bodyStart != std::string::npos) {
        std::string headers = requestText.substr(0, bodyStart);
        body = requestText.substr(bodyStart + 4);

        size_t contentLength = 0;
        size_t contentLengthPos = headers.find("Content-Length:");
        if (contentLengthPos == std::string::npos) {
            contentLengthPos = headers.find("content-length:");
        }
        if (contentLengthPos != std::string::npos) {
            contentLengthPos = headers.find(':', contentLengthPos);
            if (contentLengthPos != std::string::npos) {
                contentLength = static_cast<size_t>(std::strtoul(headers.c_str() + contentLengthPos + 1, nullptr, 10));
            }
        }

        while (body.size() < contentLength) {
            char bodyBuffer[4096] = {};
            int more = recv(client, bodyBuffer, sizeof(bodyBuffer), 0);
            if (more <= 0) break;
            body.append(bodyBuffer, bodyBuffer + more);
        }
    }

    std::string response = buildResponse(method, path, body);
    send(client, response.data(), static_cast<int>(response.size()), 0);
    shutdown(client, SD_BOTH);
    closesocket(client);
}

std::string StatusHttpServer::buildResponse(const std::string& method, const std::string& path, const std::string& requestBody) const {
    std::string status = "200 OK";
    std::string contentType = "application/json; charset=utf-8";
    std::string body;

    if (method == "POST" && path.rfind("/control/", 0) == 0) {
        if (m_controlFn) {
            body = m_controlFn(path, requestBody);
        } else {
            status = "503 Service Unavailable";
            body = "{\"ok\":false,\"error\":\"control unavailable\"}";
        }
    } else if (method != "GET") {
        status = "405 Method Not Allowed";
        body = "{\"error\":\"method not allowed\"}";
    } else if (path == "/health") {
        body = "{\"ok\":true,\"app\":\"PocketVoice\"}";
    } else if (path == "/status") {
        m_runtime.setRecentLogs(pcLogger().recentEntries());
        body = toStatusJson(m_runtime.snapshot());
    } else if (path == "/") {
        contentType = "text/html; charset=utf-8";
        body = pcStatusPageHtml();
    } else {
        status = "404 Not Found";
        body = "{\"error\":\"not found\"}";
    }

    std::ostringstream out;
    out << "HTTP/1.1 " << status << "\r\n";
    out << "Content-Type: " << contentType << "\r\n";
    out << "Content-Length: " << body.size() << "\r\n";
    out << "Cache-Control: no-store\r\n";
    out << "Connection: close\r\n";
    out << "\r\n";
    out << body;
    return out.str();
}

}
