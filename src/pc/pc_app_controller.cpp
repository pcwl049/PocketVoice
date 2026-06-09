#include "pc_app_controller.h"

#include <utility>

namespace stt {

PcAppController::PcAppController(PcRuntime& runtime) : m_runtime(runtime) {}

void PcAppController::setReconnectFn(ReconnectFn reconnectFn) {
    m_reconnectFn = std::move(reconnectFn);
}

PcControlResult PcAppController::startListening() {
    m_runtime.setListeningActive(true);
    return {true, "listen.start", "listening started"};
}

PcControlResult PcAppController::stopListening() {
    m_runtime.setListeningActive(false);
    return {true, "listen.stop", "listening stopped"};
}

PcControlResult PcAppController::reconnectPhone() {
    if (!m_reconnectFn) {
        m_runtime.setLastError("Reconnect unavailable");
        return {false, "phone.reconnect", "reconnect unavailable"};
    }

    m_runtime.setReconnecting(true);
    bool ok = m_reconnectFn();
    m_runtime.setPhoneConnected(ok);
    m_runtime.setReconnecting(false);
    if (!ok) {
        m_runtime.setLastError("Phone reconnect failed");
        return {false, "phone.reconnect", "phone reconnect failed"};
    }
    return {true, "phone.reconnect", "phone reconnected"};
}

static std::string escapeJsonString(const std::string& value) {
    std::string out;
    for (char ch : value) {
        if (ch == '\\') out += "\\\\";
        else if (ch == '"') out += "\\\"";
        else out += ch;
    }
    return out;
}

std::string toControlJson(const PcControlResult& result) {
    std::string json = "{\"ok\":";
    json += result.ok ? "true" : "false";
    json += ",\"action\":\"" + escapeJsonString(result.action) + "\"";
    json += ",\"message\":\"" + escapeJsonString(result.message) + "\"}";
    return json;
}

}
