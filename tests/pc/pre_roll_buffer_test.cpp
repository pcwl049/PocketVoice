#include "audio/pre_roll_buffer.h"

#include <cassert>
#include <cstdio>
#include <vector>

int main() {
    stt::PreRollBuffer buffer(10);
    buffer.setCapacitySeconds(1.0f);

    std::vector<float> first{0, 1, 2, 3, 4, 5};
    buffer.append(first.data(), first.size());
    assert(buffer.sampleCount() == 6);
    assert(buffer.currentTimeSeconds() == 0.6f);

    std::vector<float> second{6, 7, 8, 9, 10, 11};
    buffer.append(second.data(), second.size());
    assert(buffer.sampleCount() == 10);
    assert(buffer.currentTimeSeconds() == 1.2f);

    auto range = buffer.getRange(0.3f, 0.8f);
    assert(range.size() == 5);
    assert(range[0] == 3);
    assert(range[4] == 7);

    auto clipped = buffer.getRange(0.0f, 0.4f);
    assert(clipped.size() == 2);
    assert(clipped[0] == 2);
    assert(clipped[1] == 3);

    puts("pre_roll_buffer tests passed");
    return 0;
}
