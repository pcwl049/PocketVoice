#pragma once

#include <string>
#include <vector>
#include <functional>

namespace stt {

struct RecognizeResult {
    std::string text;
    std::string emotion = "NEUTRAL";
    std::string event = "Speech";
    bool success = false;
};

struct SenseVoiceMetadata {
    std::string emotion = "NEUTRAL";
    std::string event = "Speech";
};

SenseVoiceMetadata parseSenseVoiceMetadata(const std::string& json);

enum class BackendType {
    Unknown,
    Paraformer,
    ZipformerCtc,
    SenseVoiceQnn
};

class SttEngine {
public:
    SttEngine();
    ~SttEngine();
    
    bool init(const std::string& modelDir, const std::string& qnnLibDir);
    
    RecognizeResult recognize(const float* samples, size_t numSamples);
    bool isInitialized() const;
    BackendType backendType() const;
    const std::string& backendName() const;
    
private:
    struct Impl;
    Impl* m_impl = nullptr;
    bool m_initialized = false;
    BackendType m_backendType = BackendType::Unknown;
    std::string m_backendName = "unknown";
};

}
