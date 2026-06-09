#pragma once

#include <string>

namespace stt {

struct AdbForwardResult {
    bool ok = false;
    bool adb_found = false;
    bool device_found = false;
    bool unauthorized = false;
    std::string adb_path;
    std::string message;
};

AdbForwardResult ensureAdbForward(int port);

}
