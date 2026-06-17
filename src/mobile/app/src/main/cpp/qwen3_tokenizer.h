#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace stt {

class Qwen3Tokenizer {
public:
    Qwen3Tokenizer();
    ~Qwen3Tokenizer();
    
    /**
     * Load tokenizer from directory.
     * @param tokenizerDir Path to tokenizer directory
     * @return true if loading succeeds
     */
    bool load(const std::string& tokenizerDir);

    std::string decode(const std::vector<int>& tokenIds) const;
    std::string decode(const std::vector<int64_t>& tokenIds) const;
    std::vector<int64_t> encode(const std::string& text) const;
    int getTokenId(const std::string& token) const;
    std::string getStreamingTokenString(int tokenId, std::string* pendingBytes) const;

    int getEosTokenId() const;

    bool isLoaded() const;

private:
    struct Impl;
    Impl* m_impl = nullptr;
    bool m_loaded = false;
};

} // namespace stt
