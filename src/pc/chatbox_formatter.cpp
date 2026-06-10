#include "chatbox_formatter.h"

#include <algorithm>

namespace stt {

namespace {
constexpr const char* kVoiceTranscriptPrefix = "\xe2\x8c\x81 ";
constexpr const char* kLegacyVoiceTranscriptPrefix = "\xe2\x8c\x81 \xe5\xa3\xb0\xe9\x9f\xb3\xe8\xbd\xac\xe5\x86\x99\xef\xbc\x9a";
}

void ChatBoxFormatter::setMaxChars(size_t maxChars) {
    m_maxChars = std::max<size_t>(8, maxChars);
}

size_t ChatBoxFormatter::nextUtf8CharLen(const std::string& text, size_t offset) {
    if (offset >= text.size()) return 0;
    unsigned char c = static_cast<unsigned char>(text[offset]);
    if (c < 0x80) return 1;
    if ((c & 0xe0) == 0xc0 && offset + 1 < text.size()) return 2;
    if ((c & 0xf0) == 0xe0 && offset + 2 < text.size()) return 3;
    if ((c & 0xf8) == 0xf0 && offset + 3 < text.size()) return 4;
    return 1;
}

bool ChatBoxFormatter::isSentenceEnd(const std::string& token) {
    return token == "." || token == "!" || token == "?" || token == ";" ||
           token == "。" || token == "！" || token == "？" || token == "；";
}

void ChatBoxFormatter::pushChunk(std::vector<std::string>& chunks, const std::string& chunk) const {
    if (chunk.empty()) return;
    size_t start = 0;
    while (start < chunk.size()) {
        size_t end = start;
        size_t chars = 0;
        while (end < chunk.size() && chars < m_maxChars) {
            end += nextUtf8CharLen(chunk, end);
            chars++;
        }
        chunks.push_back(chunk.substr(start, end - start));
        start = end;
    }
}

std::vector<std::string> ChatBoxFormatter::splitText(const std::string& text) const {
    std::vector<std::string> chunks;
    std::string current;
    current.reserve(std::min(m_maxChars, text.size()));

    size_t currentChars = 0;
    for (size_t i = 0; i < text.size(); ) {
        size_t charLen = nextUtf8CharLen(text, i);
        std::string token = text.substr(i, charLen);
        current += token;
        currentChars++;
        i += charLen;
        bool boundary = isSentenceEnd(token);
        bool tooLong = currentChars >= m_maxChars;
        if (boundary || tooLong) {
            pushChunk(chunks, current);
            current.clear();
            currentChars = 0;
        }
    }
    pushChunk(chunks, current);
    return chunks;
}

std::string ChatBoxFormatter::formatMessage(const std::string& text) const {
    if (text.empty()) return "";
    const std::string prefix = kVoiceTranscriptPrefix;
    const std::string legacyPrefix = kLegacyVoiceTranscriptPrefix;
    if (text.rfind(legacyPrefix, 0) == 0) {
        return prefix + text.substr(legacyPrefix.size());
    }
    if (text.rfind(prefix, 0) == 0) {
        return text;
    }
    return prefix + text;
}

bool ChatBoxFormatter::shouldSend(const std::string& text) {
    if (text.empty()) return false;
    if (std::find(m_recent.begin(), m_recent.end(), text) != m_recent.end()) {
        return false;
    }
    m_recent.push_back(text);
    if (m_recent.size() > 8) {
        m_recent.pop_front();
    }
    return true;
}

}
