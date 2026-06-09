#include "pc_app_controller.h"

#include <cassert>
#include <cstdio>

int main() {
    stt::PcRuntime runtime;
    stt::PcAppController controller(runtime);

    auto start = controller.startListening();
    assert(start.ok);
    assert(start.action == "listen.start");
    assert(runtime.snapshot().listening_active);

    auto stop = controller.stopListening();
    assert(stop.ok);
    assert(stop.action == "listen.stop");
    assert(!runtime.snapshot().listening_active);

    bool reconnectCalled = false;
    controller.setReconnectFn([&]() {
        reconnectCalled = true;
        return true;
    });
    auto reconnect = controller.reconnectPhone();
    assert(reconnect.ok);
    assert(reconnectCalled);
    assert(runtime.snapshot().phone_connected);
    assert(!runtime.snapshot().reconnecting);

    std::string json = stt::toControlJson(reconnect);
    assert(json.find("\"ok\":true") != std::string::npos);
    assert(json.find("\"action\":\"phone.reconnect\"") != std::string::npos);

    puts("pc_app_controller tests passed");
    return 0;
}
