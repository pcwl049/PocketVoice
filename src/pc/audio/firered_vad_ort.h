#pragma once

#include <string>
#include <vector>

namespace stt {

struct FireRedVadFrame {
    std::vector<float> samples;
    float confidence = 0.0f;
    bool is_speech = false;
};

class FireRedVadOrt {
public:
    FireRedVadOrt();
    ~FireRedVadOrt();

    bool init(const std::string& modelDir, float speechThreshold);
    std::vector<FireRedVadFrame> process(const float* samples, size_t numSamples);
    void reset();

private:
    struct Impl;
    Impl* m_impl = nullptr;
};

}  // namespace stt
