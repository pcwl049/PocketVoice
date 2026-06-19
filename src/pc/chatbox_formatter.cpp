#include "chatbox_formatter.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <vector>

namespace stt {

namespace {
constexpr const char* kVoiceTranscriptPrefix = "\xe2\x8c\x81 ";
constexpr const char* kLegacyVoiceTranscriptPrefix = "\xe2\x8c\x81 \xe5\xa3\xb0\xe9\x9f\xb3\xe8\xbd\xac\xe5\x86\x99\xef\xbc\x9a";

std::string lowerAsciiCopy(const std::string& text) {
    std::string lowered = text;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return lowered;
}

size_t utf8CharLenLocal(const std::string& text, size_t offset) {
    if (offset >= text.size()) return 0;
    unsigned char c = static_cast<unsigned char>(text[offset]);
    if (c < 0x80) return 1;
    if ((c & 0xe0) == 0xc0 && offset + 1 < text.size()) return 2;
    if ((c & 0xf0) == 0xe0 && offset + 2 < text.size()) return 3;
    if ((c & 0xf8) == 0xf0 && offset + 3 < text.size()) return 4;
    return 1;
}

std::string starMask(size_t charCount) {
    return std::string((std::max)(charCount, static_cast<size_t>(1)), '*');
}

std::vector<std::string> splitUtf8Chars(const std::string& text) {
    std::vector<std::string> chars;
    for (size_t i = 0; i < text.size();) {
        size_t len = utf8CharLenLocal(text, i);
        if (len == 0) break;
        chars.push_back(text.substr(i, len));
        i += len;
    }
    return chars;
}

std::string softMaskUtf8(const std::string& text) {
    const auto chars = splitUtf8Chars(text);
    if (chars.empty()) return "";
    if (chars.size() == 1) return "*";
    if (chars.size() == 2) return chars[0] + "*";
    if (chars.size() == 3) return chars[0] + "*" + chars[2];

    std::string masked = chars.front();
    for (size_t i = 1; i + 1 < chars.size(); ++i) {
        masked += "*";
    }
    masked += chars.back();
    return masked;
}

void replaceLiteral(std::string& text, const std::string& needle) {
    if (needle.empty()) return;
    const std::string replacement = softMaskUtf8(needle);
    size_t pos = 0;
    while ((pos = text.find(needle, pos)) != std::string::npos) {
        text.replace(pos, needle.size(), replacement);
        pos += replacement.size();
    }
}

void replaceAsciiCaseInsensitive(std::string& text, const std::string& needle) {
    if (needle.empty()) return;
    const std::string loweredNeedle = lowerAsciiCopy(needle);
    const std::string replacement = needle.size() <= 2
        ? starMask(needle.size())
        : std::string(1, needle[0]) + std::string(needle.size() - 1, '*');
    size_t pos = 0;
    while (pos < text.size()) {
        std::string loweredText = lowerAsciiCopy(text);
        size_t hit = loweredText.find(loweredNeedle, pos);
        if (hit == std::string::npos) break;
        text.replace(hit, needle.size(), replacement);
        pos = hit + replacement.size();
    }
}

bool isAsciiWord(const std::string& text) {
    if (text.empty()) return false;
    for (unsigned char ch : text) {
        if (ch >= 0x80) return false;
        if (!(std::isalnum(ch) || ch == '_' || ch == '-' || ch == '\'')) return false;
    }
    return true;
}

}  // namespace

void ChatBoxFormatter::setMaxChars(size_t maxChars) {
    m_maxChars = std::max<size_t>(8, maxChars);
}

void ChatBoxFormatter::loadWordList(const std::string& path) {
    m_blockedUtf8Words.clear();
    m_blockedAsciiWords.clear();

    std::ifstream in(path);
    if (in) {
        std::string line;
        while (std::getline(in, line)) {
            line = trim(line);
            if (line.empty() || line[0] == '#') continue;
            if (isAsciiWord(line)) {
                m_blockedAsciiWords.push_back(lowerAsciiCopy(line));
            } else {
                m_blockedUtf8Words.push_back(line);
            }
        }
    }

    if (!m_blockedUtf8Words.empty() || !m_blockedAsciiWords.empty()) {
        return;
    }

    constexpr std::array<const char*, 6> kDefaultUtf8Words = {
        "\xe6\x93\x8d\xe4\xbd\xa0\xe5\xa6\x88",
        "\xe8\x8d\x89\xe4\xbd\xa0\xe5\xa6\x88",
        "\xe5\x82\xbb\xe9\x80\xbc",
        "\xe7\x85\x9e\xe7\xac\x94",
        "\xe6\xb2\x99\xe6\xaf\x94",
        "\xe8\xb4\xb1\xe4\xba\xba"
    };
    constexpr std::array<const char*, 6> kDefaultAsciiWords = {
        "fuck", "fucking", "motherfucker", "bitch", "asshole", "cunt"
    };
    m_blockedUtf8Words.assign(kDefaultUtf8Words.begin(), kDefaultUtf8Words.end());
    for (const char* word : kDefaultAsciiWords) {
        m_blockedAsciiWords.push_back(word);
    }
}

std::string ChatBoxFormatter::trim(const std::string& text) {
    size_t start = 0;
    while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start]))) {
        start++;
    }
    size_t end = text.size();
    while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        end--;
    }
    return text.substr(start, end - start);
}

size_t ChatBoxFormatter::nextUtf8CharLen(const std::string& text, size_t offset) {
    return utf8CharLenLocal(text, offset);
}

bool ChatBoxFormatter::isSentenceEnd(const std::string& token) {
    return token == "." || token == "!" || token == "?" || token == ";" ||
           token == "\xe3\x80\x82" || token == "\xef\xbc\x81" ||
           token == "\xef\xbc\x9f" || token == "\xef\xbc\x9b";
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
    for (size_t i = 0; i < text.size();) {
        size_t charLen = nextUtf8CharLen(text, i);
        std::string token = text.substr(i, charLen);
        current += token;
        currentChars++;
        i += charLen;
        const bool boundary = isSentenceEnd(token);
        const bool tooLong = currentChars >= m_maxChars;
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

std::string ChatBoxFormatter::sanitizeText(const std::string& text) const {
    if (text.empty()) return "";

    std::string sanitized = text;
    for (const auto& word : m_blockedUtf8Words) {
        replaceLiteral(sanitized, word);
    }
    for (const auto& word : m_blockedAsciiWords) {
        replaceAsciiCaseInsensitive(sanitized, word);
    }
    return sanitized;
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

}  // namespace stt
