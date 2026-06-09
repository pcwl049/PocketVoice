#include "embedded_vad_model.h"

#include <cassert>
#include <cstdio>
#include <filesystem>

int main() {
    std::filesystem::path path = stt::embeddedSileroVadPath();
    assert(!path.empty());
    assert(std::filesystem::exists(path));
    assert(std::filesystem::file_size(path) == 643854);
    assert(path.filename().string().find("silero_vad") != std::string::npos);

    std::filesystem::path second = stt::embeddedSileroVadPath();
    assert(second == path);

    puts("embedded_vad_model tests passed");
    return 0;
}
