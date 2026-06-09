#include "stt_engine.h"

#include <cassert>
#include <cstdio>

int main() {
    auto metadata = stt::parseSenseVoiceMetadata(
        "{\"lang\":\"zh\",\"emotion\":\"<|HAPPY|>\",\"event\":\"<|Speech|>\",\"text\":\"你好。\"}");
    assert(metadata.emotion == "HAPPY");
    assert(metadata.event == "Speech");

    metadata = stt::parseSenseVoiceMetadata(
        "{\"emotion\":\"\",\"event\":\"\",\"text\":\"无标签\"}");
    assert(metadata.emotion == "NEUTRAL");
    assert(metadata.event == "Speech");

    puts("sensevoice_metadata tests passed");
    return 0;
}
