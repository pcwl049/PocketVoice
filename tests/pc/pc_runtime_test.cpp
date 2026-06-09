#include "pc_runtime.h"

#include <cassert>
#include <cstdio>

int main() {
    stt::PcRuntime runtime;
    runtime.setRunning(true);
    runtime.setPhoneConnected(true);
    runtime.setOscReady(true);
    runtime.setTypingActive(true);
    runtime.setChatBoxDryRun(false);
    runtime.setListeningActive(true);
    runtime.setReconnecting(true);
    runtime.incrementSentAudioCount();
    runtime.setLastText("hello", "NEUTRAL");
    runtime.setLastError("boom");
    runtime.setRecentLogs({{1234, stt::PcLogLevel::Warning, "Network", "retry"}});

    stt::ChatBoxQueueSnapshot queue;
    queue.pending_count = 2;
    queue.sent_count = 1;
    queue.last_sent_text = "hello";
    runtime.setChatBoxSnapshot(queue);

    auto snapshot = runtime.snapshot();
    assert(snapshot.running);
    assert(snapshot.phone_connected);
    assert(snapshot.osc_ready);
    assert(snapshot.typing_active);
    assert(!snapshot.chatbox_dry_run);
    assert(snapshot.listening_active);
    assert(snapshot.reconnecting);
    assert(snapshot.sent_audio_count == 1);
    assert(snapshot.last_text == "hello");
    assert(snapshot.last_emotion == "NEUTRAL");
    assert(snapshot.last_error == "boom");
    assert(snapshot.recent_logs.size() == 1);
    assert(snapshot.recent_logs[0].level == stt::PcLogLevel::Warning);
    assert(snapshot.recent_logs[0].category == "Network");
    assert(snapshot.recent_logs[0].message == "retry");
    assert(snapshot.chatbox.pending_count == 2);
    assert(snapshot.chatbox.sent_count == 1);
    assert(snapshot.chatbox.last_sent_text == "hello");

    runtime.clearLastError();
    assert(runtime.snapshot().last_error.empty());

    puts("pc_runtime tests passed");
    return 0;
}
