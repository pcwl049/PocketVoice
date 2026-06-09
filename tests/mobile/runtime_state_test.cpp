#include "runtime_state.h"

#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

int main() {
    stt::RuntimeState state;
    std::vector<float> audio{0.1f, -0.2f, 0.3f, 0.4f};

    auto miss = state.findCached(audio.data(), audio.size());
    assert(!miss.hit);

    state.recordRecognition(audio.data(), audio.size(), 16000, 18, "hello");
    auto snapshot = state.snapshot();

    assert(snapshot.lastText == "hello");
    assert(snapshot.lastAudioMs == 0);
    assert(snapshot.lastRecognizeMs == 18);
    assert(snapshot.lastUpdatedMs > 0);
    assert(snapshot.history.size() == 1);
    assert(snapshot.history[0].text == "hello");
    assert(snapshot.history[0].recognizeMs == 18);
    assert(snapshot.history[0].cacheHit == false);

    auto hit = state.findCached(audio.data(), audio.size());
    assert(hit.hit);
    assert(hit.text == "hello");

    state.recordCacheHit(audio.data(), audio.size(), 16000, hit.text);
    snapshot = state.snapshot();
    assert(snapshot.cacheHits == 1);
    assert(snapshot.history.size() == 2);
    assert(snapshot.history[0].cacheHit == true);
    assert(snapshot.history[0].recognizeMs == 0);

    puts("runtime_state tests passed");
    return 0;
}
