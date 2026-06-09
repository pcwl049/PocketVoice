#pragma once

#include "pc_runtime.h"

#include <functional>
#include <string>

namespace stt {

struct PcControlResult {
    bool ok = false;
    std::string action;
    std::string message;
};

class PcAppController {
public:
    using ReconnectFn = std::function<bool()>;

    explicit PcAppController(PcRuntime& runtime);

    void setReconnectFn(ReconnectFn reconnectFn);
    PcControlResult startListening();
    PcControlResult stopListening();
    PcControlResult reconnectPhone();

private:
    PcRuntime& m_runtime;
    ReconnectFn m_reconnectFn;
};

std::string toControlJson(const PcControlResult& result);

}
