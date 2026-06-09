#include "audio_job_queue.h"

#include <cassert>
#include <cstdio>
#include <thread>
#include <vector>

static stt::AudioData makeAudio(int marker) {
    stt::AudioData audio;
    audio.sample_rate = 16000;
    audio.channels = 1;
    audio.is_final = true;
    audio.samples = {static_cast<float>(marker)};
    return audio;
}

int main() {
    {
        stt::AudioJobQueue queue(2);
        assert(queue.tryPush(makeAudio(1)));
        assert(queue.tryPush(makeAudio(2)));
        assert(!queue.tryPush(makeAudio(3)));
        assert(queue.droppedJobs() == 1);
        assert(queue.size() == 2);

        stt::AudioData first;
        stt::AudioData second;
        assert(queue.waitPop(first));
        assert(queue.waitPop(second));
        assert(first.samples[0] == 1.0f);
        assert(second.samples[0] == 2.0f);
    }

    {
        stt::AudioJobQueue queue(1);
        assert(queue.tryPush(makeAudio(4)));
        queue.clear();
        assert(queue.size() == 0);
        assert(queue.tryPush(makeAudio(5)));
        stt::AudioData audio;
        assert(queue.waitPop(audio));
        assert(audio.samples[0] == 5.0f);
    }

    {
        stt::AudioJobQueue queue(1);
        bool woke = false;
        std::thread waiter([&]() {
            stt::AudioData audio;
            woke = !queue.waitPop(audio);
        });
        queue.stop();
        waiter.join();
        assert(woke);
    }

    puts("audio_job_queue tests passed");
    return 0;
}
