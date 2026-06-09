#include "chatbox_formatter.h"

#include <cassert>
#include <cstdio>

int main() {
    stt::ChatBoxFormatter formatter;
    formatter.setMaxChars(18);

    auto parts = formatter.splitText("第一句。第二句很长很长很长，需要切开！第三句");
    assert(parts.size() >= 3);
    assert(parts[0] == "第一句。");
    for (const auto& part : parts) {
        assert(part.find("\xef\xbf\xbd") == std::string::npos);
    }

    assert(formatter.shouldSend("第一句。"));
    assert(!formatter.shouldSend("第一句。"));
    assert(formatter.shouldSend("第二句。"));

    puts("chatbox_formatter tests passed");
    return 0;
}
