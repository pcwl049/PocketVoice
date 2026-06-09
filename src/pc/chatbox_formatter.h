#pragma once

#include <deque>
#include <string>
#include <vector>

namespace stt {

class ChatBoxFormatter {
public:
    void setMaxChars(size_t maxChars);
    std::vector<std::string> splitText(const std::string& text) const;
    std::string formatMessage(const std::string& text) const;
    bool shouldSend(const std::string& text);

private:
    static size_t nextUtf8CharLen(const std::string& text, size_t offset);
    static bool isSentenceEnd(const std::string& token);
    void pushChunk(std::vector<std::string>& chunks, const std::string& chunk) const;

    size_t m_maxChars = 144;
    std::deque<std::string> m_recent;
};

}
