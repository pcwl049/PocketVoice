#include "pc_status_page.h"

#include <cassert>
#include <cstdio>

int main() {
    std::string html = stt::pcStatusPageHtml();
    assert(html.find("<!doctype html>") != std::string::npos);
    assert(html.find("PocketVoice Console") != std::string::npos);
    assert(html.find("fetch(\"/status\"") != std::string::npos);
    assert(html.find("最近识别文本") != std::string::npos);
    assert(html.find("ChatBox 队列") != std::string::npos);
    assert(html.find("/control/queue/pause") != std::string::npos);
    assert(html.find("/control/queue/resume") != std::string::npos);
    assert(html.find("/control/queue/clear") != std::string::npos);
    assert(html.find("/control/chatbox/clear") != std::string::npos);
    assert(html.find("/control/error/clear") != std::string::npos);
    assert(html.find("/control/listen/stop") != std::string::npos);
    assert(html.find("/control/listen/start") != std::string::npos);
    assert(html.find("/control/phone/reconnect") != std::string::npos);
    assert(html.find("audio-device-select") != std::string::npos);
    assert(html.find("/control/audio/input-device") != std::string::npos);
    assert(html.find("data.audio_input") != std::string::npos);
    assert(html.find("renderAudioDevices") != std::string::npos);
    puts("pc_status_page tests passed");
    return 0;
}
