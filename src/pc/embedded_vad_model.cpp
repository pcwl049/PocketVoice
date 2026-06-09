#include "embedded_vad_model.h"

#include "resource.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <fstream>
#include <mutex>
#include <stdexcept>

namespace stt {

namespace {

static std::once_flag g_extractOnce;
static std::filesystem::path g_extractedPath;

static std::filesystem::path tempModelPath() {
    wchar_t tempDir[MAX_PATH] = {};
    DWORD len = GetTempPathW(MAX_PATH, tempDir);
    if (len == 0 || len >= MAX_PATH) {
        throw std::runtime_error("GetTempPathW failed");
    }

    std::filesystem::path dir(tempDir);
    dir /= L"PocketVoice";
    std::filesystem::create_directories(dir);
    return dir / L"silero_vad.onnx";
}

static void extractEmbeddedSileroVad() {
    HMODULE module = GetModuleHandleW(nullptr);
    HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(IDR_SILERO_VAD_ONNX), L"STT_SILERO_VAD_ONNX");
    if (!resource) {
        throw std::runtime_error("FindResource STT_SILERO_VAD_ONNX failed");
    }

    HGLOBAL loaded = LoadResource(module, resource);
    DWORD size = SizeofResource(module, resource);
    const void* data = LockResource(loaded);
    if (!loaded || !data || size == 0) {
        throw std::runtime_error("LoadResource STT_SILERO_VAD_ONNX failed");
    }

    auto path = tempModelPath();
    if (std::filesystem::exists(path) && std::filesystem::file_size(path) == size) {
        g_extractedPath = path;
        return;
    }

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("Failed to create embedded VAD model file");
    }
    out.write(static_cast<const char*>(data), size);
    if (!out.good()) {
        throw std::runtime_error("Failed to write embedded VAD model file");
    }

    g_extractedPath = path;
}

}

std::filesystem::path embeddedSileroVadPath() {
    std::call_once(g_extractOnce, extractEmbeddedSileroVad);
    return g_extractedPath;
}

}
