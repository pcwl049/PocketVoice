#include "vad.h"
#include "firered_vad_ort.h"
#include "../pc_logger.h"

#include <cstring>
#include <cmath>
#include <string>

extern "C" {
#include "sherpa-onnx/c-api/c-api.h"
}

namespace stt {

struct Vad::Impl {
    const SherpaOnnxVoiceActivityDetector* vad = nullptr;
    FireRedVadOrt* fireRed = nullptr;
    std::string backend = "silero";
};

Vad::Vad() : m_impl(new Impl()) {}

Vad::~Vad() {
    if (m_impl->vad) {
        SherpaOnnxDestroyVoiceActivityDetector(m_impl->vad);
    }
    delete m_impl->fireRed;
    delete m_impl;
}

bool Vad::init(const std::string& modelPath, float speechThreshold, float endSilenceDuration,
               float minSpeechDuration, float maxSpeechDuration,
               const std::string& backend, const std::string& fallbackModelPath) {
    m_threshold = speechThreshold;
    m_endSilenceDuration = endSilenceDuration;
    m_minSpeechDuration = minSpeechDuration;
    m_maxSpeechDuration = maxSpeechDuration;

    if (m_impl->vad) {
        SherpaOnnxDestroyVoiceActivityDetector(m_impl->vad);
        m_impl->vad = nullptr;
    }
    delete m_impl->fireRed;
    m_impl->fireRed = nullptr;

    if (backend == "firered") {
        m_impl->fireRed = new FireRedVadOrt();
        if (m_impl->fireRed->init(modelPath, speechThreshold)) {
            m_impl->backend = "firered";
            pcLogf(PcLogLevel::Info,
                   "VAD",
                   "Initialized FireRedVAD with model dir: %s, speech_threshold: %.2f, end_silence: %.2f, max_speech: %.2f",
                   modelPath.c_str(),
                   speechThreshold,
                   endSilenceDuration,
                   maxSpeechDuration);
            return true;
        }
        delete m_impl->fireRed;
        m_impl->fireRed = nullptr;
        pcLog(PcLogLevel::Warning, "VAD", "FireRedVAD unavailable, falling back to Silero VAD");
    }

    const std::string sileroModelPath = fallbackModelPath.empty() ? modelPath : fallbackModelPath;
    
    SherpaOnnxSileroVadModelConfig sileroConfig;
    memset(&sileroConfig, 0, sizeof(sileroConfig));
    
    sileroConfig.model = sileroModelPath.c_str();
    sileroConfig.threshold = speechThreshold;
    sileroConfig.min_silence_duration = endSilenceDuration;
    sileroConfig.min_speech_duration = m_minSpeechDuration;
    sileroConfig.max_speech_duration = m_maxSpeechDuration;
    sileroConfig.window_size = 512;
    
    SherpaOnnxVadModelConfig config;
    memset(&config, 0, sizeof(config));
    config.silero_vad = sileroConfig;
    config.sample_rate = m_sampleRate;
    config.num_threads = 2;
    config.provider = "cpu";
    config.debug = 0;
    
    m_impl->vad = SherpaOnnxCreateVoiceActivityDetector(&config, 30.0f);
    
    if (!m_impl->vad) {
        pcLog(PcLogLevel::Error, "VAD", "Failed to create VAD");
        return false;
    }
    
    pcLogf(PcLogLevel::Info,
           "VAD",
           "Initialized with model: %s, speech_threshold: %.2f, end_silence: %.2f, max_speech: %.2f",
           sileroModelPath.c_str(),
           speechThreshold,
           endSilenceDuration,
           maxSpeechDuration);
    m_impl->backend = "silero";
    return true;
}

void Vad::setThreshold(float threshold) {
    m_threshold = threshold;
}

void Vad::setCallbacks(SpeechCallback speechCb, SegmentCallback segmentCb) {
    m_speechCallback = speechCb;
    m_segmentCallback = segmentCb;
}

void Vad::process(const float* samples, size_t numSamples) {
    if (m_impl->fireRed) {
        auto frames = m_impl->fireRed->process(samples, numSamples);
        for (const auto& frame : frames) {
            updateSpeechState(frame.is_speech, frame.samples.data(), frame.samples.size(),
                              frame.samples.empty() ? 0.0f : (float)frame.samples.size() / m_sampleRate);
        }
        return;
    }

    if (!m_impl->vad) return;

    SherpaOnnxVoiceActivityDetectorAcceptWaveform(m_impl->vad, samples, (int32_t)numSamples);
    
    bool isSpeech = SherpaOnnxVoiceActivityDetectorDetected(m_impl->vad) != 0;
    
    float chunkDuration = (float)numSamples / m_sampleRate;
    updateSpeechState(isSpeech, samples, numSamples, chunkDuration);

    while (!SherpaOnnxVoiceActivityDetectorEmpty(m_impl->vad)) {
        const SherpaOnnxSpeechSegment* segment = SherpaOnnxVoiceActivityDetectorFront(m_impl->vad);
        if (segment) {
            if (m_segmentCallback) {
                SpeechSegment seg;
                seg.samples.assign(segment->samples, segment->samples + segment->n);
                seg.start_time = segment->start / (float)m_sampleRate;
                seg.duration = segment->n / (float)m_sampleRate;
                m_segmentCallback(seg);
            }
            SherpaOnnxDestroySpeechSegment(segment);
        }
        SherpaOnnxVoiceActivityDetectorPop(m_impl->vad);
    }
}

void Vad::updateSpeechState(bool isSpeech, const float* samples, size_t numSamples, float chunkDuration) {
    m_currentTime += chunkDuration;

    if (isSpeech && !m_inSpeech) {
        m_inSpeech = true;
        m_speechStart = m_currentTime - chunkDuration;
        m_silenceDuration = 0.0f;
        pcLogf(PcLogLevel::Info, "VAD", "Speech started at %.3f", m_speechStart);
    } else if (!isSpeech && m_inSpeech) {
        m_silenceDuration += chunkDuration;
        
        if (m_silenceDuration >= m_endSilenceDuration) {
            detectSegment();
            m_inSpeech = false;
            pcLogf(PcLogLevel::Info, "VAD", "Speech ended at %.3f, duration %.3f", m_currentTime, m_currentTime - m_speechStart);
        }
    }
    
    if (m_inSpeech && (m_impl->fireRed || m_speechCallback)) {
        m_buffer.insert(m_buffer.end(), samples, samples + numSamples);
        if (m_speechCallback) {
            m_speechCallback(samples, numSamples);
        }
        
        if (m_buffer.size() > (size_t)(m_maxSpeechDuration * m_sampleRate)) {
            detectSegment();
        }
    }
}

void Vad::detectSegment() {
    if (m_buffer.empty()) return;
    
    if (m_segmentCallback) {
        SpeechSegment seg;
        seg.samples = m_buffer;
        seg.start_time = m_speechStart;
        seg.duration = (float)m_buffer.size() / m_sampleRate;
        m_segmentCallback(seg);
    }
    
    m_buffer.clear();
}

void Vad::flush() {
    if (m_impl->fireRed) {
        if (m_inSpeech && !m_buffer.empty()) {
            detectSegment();
            m_inSpeech = false;
        }
        return;
    }

    if (!m_impl->vad) return;

    SherpaOnnxVoiceActivityDetectorFlush(m_impl->vad);
    
    if (m_inSpeech && !m_buffer.empty()) {
        detectSegment();
        m_inSpeech = false;
    }
    
    while (!SherpaOnnxVoiceActivityDetectorEmpty(m_impl->vad)) {
        const SherpaOnnxSpeechSegment* segment = SherpaOnnxVoiceActivityDetectorFront(m_impl->vad);
        if (segment) {
            if (m_segmentCallback) {
                SpeechSegment seg;
                seg.samples.assign(segment->samples, segment->samples + segment->n);
                seg.start_time = segment->start / (float)m_sampleRate;
                seg.duration = segment->n / (float)m_sampleRate;
                m_segmentCallback(seg);
            }
            SherpaOnnxDestroySpeechSegment(segment);
        }
        SherpaOnnxVoiceActivityDetectorPop(m_impl->vad);
    }
}

void Vad::reset() {
    if (m_impl->vad) {
        SherpaOnnxVoiceActivityDetectorReset(m_impl->vad);
    }
    if (m_impl->fireRed) {
        m_impl->fireRed->reset();
    }
    m_buffer.clear();
    m_inSpeech = false;
    m_currentTime = 0.0f;
    m_speechStart = 0.0f;
    m_silenceDuration = 0.0f;
}

bool Vad::isSpeech() const {
    if (m_impl->fireRed) return m_inSpeech;
    return m_impl->vad ? (SherpaOnnxVoiceActivityDetectorDetected(m_impl->vad) != 0) : false;
}

bool Vad::isInSpeech() const {
    return m_inSpeech;
}

}
