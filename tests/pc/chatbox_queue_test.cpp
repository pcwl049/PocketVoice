#include "chatbox_queue.h"

#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

struct FakeClock {
    int64_t nowMs = 0;
    int64_t operator()() const { return nowMs; }
};

int main() {
    FakeClock clock;
    std::vector<std::string> sent;

    stt::ChatBoxQueue queue(
        [&sent](const std::string& text) {
            sent.push_back(text);
            return true;
        },
        [&clock]() { return clock(); }
    );
    queue.setIntervalMs(1500);

    queue.enqueue({"first", "second", "second", "third"});
    auto snapshot = queue.snapshot();
    assert(snapshot.pending_count == 3);
    assert(snapshot.sent_count == 0);
    assert(snapshot.skipped_duplicate_count == 1);

    assert(queue.tick());
    assert(sent.size() == 1);
    assert(sent[0] == "first");
    snapshot = queue.snapshot();
    assert(snapshot.pending_count == 2);
    assert(snapshot.sent_count == 1);
    assert(snapshot.last_sent_text == "first");

    clock.nowMs = 1000;
    assert(!queue.tick());
    assert(sent.size() == 1);

    clock.nowMs = 1500;
    assert(queue.tick());
    assert(sent.size() == 2);
    assert(sent[1] == "second");

    queue.setPaused(true);
    assert(queue.snapshot().paused);
    clock.nowMs = 3000;
    assert(!queue.tick());
    assert(sent.size() == 2);
    assert(queue.snapshot().pending_count == 1);

    queue.setPaused(false);
    assert(!queue.snapshot().paused);
    assert(queue.tick());
    assert(sent.size() == 3);
    assert(sent[2] == "third");

    queue.enqueue({"first", "fourth"});
    snapshot = queue.snapshot();
    assert(snapshot.pending_count == 1);
    assert(snapshot.skipped_duplicate_count == 2);

    clock.nowMs = 4500;
    assert(queue.tick());
    assert(sent.size() == 4);
    assert(sent[3] == "fourth");

    queue.enqueue({"fifth", "sixth"});
    assert(queue.snapshot().pending_count == 2);
    queue.clear();
    snapshot = queue.snapshot();
    assert(snapshot.pending_count == 0);
    assert(snapshot.sending == false);

    puts("chatbox_queue tests passed");
    return 0;
}
