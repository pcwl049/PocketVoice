#pragma once

#include <cstdint>
#include <vector>
#include <string>

namespace stt {

constexpr uint16_t MAGIC = 0x5354;
constexpr uint8_t FLAG_FINAL = 0x01;
constexpr uint8_t FLAG_CANCEL = 0x02;
constexpr uint8_t FLAG_HAS_SEGMENT_ID = 0x04;

enum class MessageType : uint8_t {
    Audio = 0x01,
    Text = 0x02,
    Heartbeat = 0x03,
    Error = 0x04,
    Config = 0x05
};

enum class Emotion : uint8_t {
    Unknown = 0,
    Happy = 1,
    Sad = 2,
    Angry = 3,
    Fear = 4,
    Surprise = 5,
    Disgust = 6,
    Neutral = 7
};

enum class AudioEvent : uint8_t {
    Unknown = 0,
    Speech = 1,
    Music = 2,
    Noise = 3,
    Silence = 4
};

struct MessageHeader {
    uint16_t magic;
    uint8_t type;
    uint8_t flags;
    uint32_t length;
};

struct AudioMessage {
    MessageHeader header;
    uint32_t sample_rate;
    uint16_t channels;
    uint32_t segment_id;
    std::vector<float> samples;
    
    AudioMessage() {
        header.magic = MAGIC;
        header.type = static_cast<uint8_t>(MessageType::Audio);
        header.flags = 0;
        sample_rate = 16000;
        channels = 1;
        segment_id = 0;
    }
    
    bool isFinal() const { return header.flags & FLAG_FINAL; }
    void setFinal(bool v) { if (v) header.flags |= FLAG_FINAL; else header.flags &= ~FLAG_FINAL; }
    
    bool isCancel() const { return header.flags & FLAG_CANCEL; }
    void setCancel(bool v) { if (v) header.flags |= FLAG_CANCEL; else header.flags &= ~FLAG_CANCEL; }

    bool hasSegmentId() const { return header.flags & FLAG_HAS_SEGMENT_ID; }
    void setHasSegmentId(bool v) { if (v) header.flags |= FLAG_HAS_SEGMENT_ID; else header.flags &= ~FLAG_HAS_SEGMENT_ID; }
};

struct TextMessage {
    MessageHeader header;
    uint32_t segment_id;
    std::string text;
    Emotion emotion;
    AudioEvent event;
    
    TextMessage() {
        header.magic = MAGIC;
        header.type = static_cast<uint8_t>(MessageType::Text);
        header.flags = 0;
        segment_id = 0;
        emotion = Emotion::Neutral;
        event = AudioEvent::Speech;
    }
    
    bool hasEmotion() const { return header.flags & 0x01; }
    void setHasEmotion(bool v) { if (v) header.flags |= 0x01; else header.flags &= ~0x01; }
    
    bool hasEvent() const { return header.flags & 0x02; }
    void setHasEvent(bool v) { if (v) header.flags |= 0x02; else header.flags &= ~0x02; }

    bool hasSegmentId() const { return header.flags & FLAG_HAS_SEGMENT_ID; }
    void setHasSegmentId(bool v) { if (v) header.flags |= FLAG_HAS_SEGMENT_ID; else header.flags &= ~FLAG_HAS_SEGMENT_ID; }
};

inline const char* emotionToString(Emotion e) {
    switch (e) {
        case Emotion::Happy: return "HAPPY";
        case Emotion::Sad: return "SAD";
        case Emotion::Angry: return "ANGRY";
        case Emotion::Fear: return "FEAR";
        case Emotion::Surprise: return "SURPRISE";
        case Emotion::Disgust: return "DISGUST";
        case Emotion::Neutral: return "NEUTRAL";
        default: return "UNKNOWN";
    }
}

inline const char* emotionToEmoji(Emotion e) {
    switch (e) {
        case Emotion::Happy: return "😊";
        case Emotion::Sad: return "😢";
        case Emotion::Angry: return "😠";
        case Emotion::Fear: return "😨";
        case Emotion::Surprise: return "😲";
        case Emotion::Disgust: return "🤢";
        case Emotion::Neutral: return "";
        default: return "";
    }
}

inline Emotion parseEmotion(const std::string& str) {
    if (str == "HAPPY" || str == "<|HAPPY|>") return Emotion::Happy;
    if (str == "SAD" || str == "<|SAD|>") return Emotion::Sad;
    if (str == "ANGRY" || str == "<|ANGRY|>") return Emotion::Angry;
    if (str == "FEAR" || str == "<|FEAR|>") return Emotion::Fear;
    if (str == "SURPRISE" || str == "<|SURPRISE|>") return Emotion::Surprise;
    if (str == "DISGUST" || str == "<|DISGUST|>") return Emotion::Disgust;
    if (str == "NEUTRAL" || str == "<|NEUTRAL|>") return Emotion::Neutral;
    return Emotion::Unknown;
}

}
