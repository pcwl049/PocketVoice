#include "audio/buffer.h"

#include <cassert>
#include <cmath>
#include <cstdio>

int main() {
    stt::AudioBuffer buffer;
    buffer.setInterSegmentSilence(0.1f);

    const float first[] = {0.1f, 0.2f, 0.3f};
    const float second[] = {-0.1f, -0.2f};

    buffer.addSegment(first, 3);
    buffer.addSegment(second, 2);

    auto merged = buffer.getMergedAudio();
    assert(merged.size() == 3 + 1600 + 2);
    assert(std::fabs(merged[0] - 0.1f) < 0.0001f);
    assert(std::fabs(merged[2] - 0.3f) < 0.0001f);
    for (size_t i = 3; i < 1603; ++i) {
        assert(merged[i] == 0.0f);
    }
    assert(std::fabs(merged[1603] + 0.1f) < 0.0001f);
    assert(std::fabs(merged[1604] + 0.2f) < 0.0001f);

    puts("audio_buffer tests passed");
    return 0;
}
