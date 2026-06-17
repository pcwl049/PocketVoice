#include "qwen3_tokenizer.h"

#ifndef STT_ENGINE_METADATA_ONLY
#include <android/log.h>
#endif

#include <memory>

#include "sherpa-onnx/csrc/qwen-asr-tokenizer.h"

#ifndef STT_ENGINE_METADATA_ONLY
#define LOG_TAG "Qwen3Tokenizer"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#else
#define LOGI(...)
#define LOGE(...)
#endif

namespace stt {

struct Qwen3Tokenizer::Impl {
    std::unique_ptr<sherpa_onnx::QwenAsrTokenizer> tokenizer;
    int eosTokenId = -1;
};

Qwen3Tokenizer::Qwen3Tokenizer() : m_impl(new Impl()) {}

Qwen3Tokenizer::~Qwen3Tokenizer() {
    delete m_impl;
}

bool Qwen3Tokenizer::load(const std::string& tokenizerDir) {
    LOGI("Loading tokenizer from: %s", tokenizerDir.c_str());

    try {
        m_impl->tokenizer = std::make_unique<sherpa_onnx::QwenAsrTokenizer>(tokenizerDir);
        m_impl->eosTokenId = static_cast<int>(m_impl->tokenizer->GetEosTokenId());
        m_loaded = true;
        LOGI("Qwen tokenizer loaded, eos=%d", m_impl->eosTokenId);
        return true;
    } catch (...) {
        LOGE("Failed to initialize sherpa-onnx QwenAsrTokenizer");
        m_impl->tokenizer.reset();
        m_impl->eosTokenId = -1;
        m_loaded = false;
        return false;
    }
}

std::string Qwen3Tokenizer::decode(const std::vector<int>& tokenIds) const {
    if (!m_loaded || !m_impl->tokenizer) {
        return "";
    }

    std::vector<int64_t> ids;
    ids.reserve(tokenIds.size());
    for (int id : tokenIds) {
        ids.push_back(static_cast<int64_t>(id));
    }

    return m_impl->tokenizer->Decode(ids);
}

std::string Qwen3Tokenizer::decode(const std::vector<int64_t>& tokenIds) const {
    if (!m_loaded || !m_impl->tokenizer) {
        return "";
    }

    return m_impl->tokenizer->Decode(tokenIds);
}

std::vector<int64_t> Qwen3Tokenizer::encode(const std::string& text) const {
    if (!m_loaded || !m_impl->tokenizer) {
        return {};
    }

    return m_impl->tokenizer->Encode(text);
}

int Qwen3Tokenizer::getTokenId(const std::string& token) const {
    if (!m_loaded || !m_impl->tokenizer) {
        return -1;
    }

    return static_cast<int>(m_impl->tokenizer->GetTokenId(token));
}

std::string Qwen3Tokenizer::getStreamingTokenString(int tokenId, std::string* pendingBytes) const {
    if (!m_loaded || !m_impl->tokenizer) {
        return "";
    }

    return m_impl->tokenizer->GetTokenStringStreaming(static_cast<int64_t>(tokenId), pendingBytes);
}

int Qwen3Tokenizer::getEosTokenId() const {
    return m_impl->eosTokenId;
}

bool Qwen3Tokenizer::isLoaded() const {
    return m_loaded;
}

} // namespace stt
