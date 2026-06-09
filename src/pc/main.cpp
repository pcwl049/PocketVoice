#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdio>
#include <string>
#include <atomic>
#include <thread>
#include <mutex>
#include <vector>
#include <fstream>
#include <condition_variable>
#include <algorithm>
#include <deque>
#include <cctype>
#include <cstdlib>

#include "../common/config.h"
#include "../common/protocol.h"
#include "audio/vad.h"
#include "audio/buffer.h"
#include "audio/pre_roll_buffer.h"
#include "audio/wasapi_capture.h"
#include "osc/sender.h"
#include "network/client.h"
#include "chatbox_formatter.h"
#include "chatbox_queue.h"
#include "pc_app_controller.h"
#include "pc_runtime.h"
#include "status_http_server.h"
#include "embedded_vad_model.h"

#pragma comment(lib, "ws2_32.lib")

static std::atomic<bool> g_running(true);
static std::mutex g_mutex;

static constexpr int kSampleRate = 16000;
static constexpr float kVadPreRollSeconds = 0.35f;
static constexpr float kVadPostRollSeconds = 0.35f;
static constexpr float kLiveRingBufferSeconds = kVadPreRollSeconds + 12.0f + kVadPostRollSeconds;

static stt::Config g_config;
static stt::Vad g_vad;
static stt::AudioBuffer g_buffer;
static stt::PreRollBuffer g_preRollBuffer(kSampleRate);
static stt::WasapiCapture g_wasapiCapture;
static stt::OscSender g_oscSender;
static stt::NetworkClient g_networkClient;
static stt::ChatBoxFormatter g_chatboxFormatter;
static stt::PcRuntime g_runtime;
static stt::PcAppController g_controller(g_runtime);
static stt::StatusHttpServer g_statusServer(g_runtime);

static std::wstring g_currentText;
static std::string g_currentEmotion;
static std::string g_lastText;
static bool g_receivedText = false;
static std::condition_variable g_textCv;
static int g_sentAudioCount = 0;
static std::atomic<uint32_t> g_nextSegmentId{1};
static const std::vector<float>* g_vadSourceSamples = nullptr;
static bool g_applyVadPadding = false;
static bool g_chatboxDryRun = false;
static bool g_oscReady = false;
static bool g_typingActive = false;
static std::thread g_chatboxThread;
static std::atomic<bool> g_chatboxThreadRunning(false);
static stt::ChatBoxQueue* g_chatboxQueue = nullptr;
static std::mutex g_chatboxQueueMutex;
static std::mutex g_audioInputMutex;
static std::string g_selectedAudioDeviceId;
static std::atomic<bool> g_restartWasapiCapture(false);
static bool g_audioLoopbackMode = false;

struct PendingLiveSegment {
    stt::SpeechSegment segment;
    std::vector<float> preRoll;
    float paddedStart = 0.0f;
    float paddedEnd = 0.0f;
};

static std::deque<PendingLiveSegment> g_pendingLiveSegments;

void onSpeechSegment(const stt::SpeechSegment& segment);
void onTextReceived(const stt::TextResult& result);
void flushPendingLiveSegments(bool force);
void addPaddedSegmentToBuffer(const stt::SpeechSegment& segment, const float* samples, size_t numSamples,
                              float paddedStart, float paddedEnd);
void setColor(int color);
static void setTyping(bool typing);
static void sendChatBoxText(const std::string& text, const std::string& emotion);
static int64_t nowMillis();
static void startChatBoxQueue();
static void stopChatBoxQueue();
static void chatBoxQueueLoop();
static void printRuntimeSnapshot(const char* label);
static void startStatusServer();
static void stopStatusServer();
static std::string handleControlRequest(const std::string& path, const std::string& body);
static std::string controlJson(bool ok, const std::string& action, const std::string& message);
static bool reconnectPhone();
static std::string resolveVadModelPath();
static void refreshAudioInputSnapshot(const std::string& mode);
static std::string selectedAudioDeviceName(const std::vector<stt::WasapiCapture::AudioInputDevice>& devices,
                                           const std::string& selectedId);
static bool setSelectedAudioDevice(const std::string& deviceId, bool requestRestart);
static std::string extractControlValue(const std::string& path, const std::string& body, const std::string& key);
static std::string urlDecode(const std::string& value);
static int listAudioDevicesAndExit();

static void clearVadPaddingSource() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_vadSourceSamples = nullptr;
    g_pendingLiveSegments.clear();
}

static int64_t nowMillis() {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

static void startChatBoxQueue() {
    if (g_chatboxQueue) return;
    g_chatboxQueue = new stt::ChatBoxQueue(
        [](const std::string& text) {
            if (!g_oscReady) return false;
            bool ok = g_oscSender.sendChatBox(text, true);
            if (ok) {
                setColor(10);
                printf("[ChatBox] %s\n", text.c_str());
                setColor(7);
            }
            return ok;
        },
        nowMillis
    );
    g_chatboxQueue->setIntervalMs(1500);
    g_runtime.setChatBoxSnapshot(g_chatboxQueue->snapshot());
    g_chatboxThreadRunning = true;
    g_chatboxThread = std::thread(chatBoxQueueLoop);
}

static void stopChatBoxQueue() {
    g_chatboxThreadRunning = false;
    if (g_chatboxThread.joinable()) {
        g_chatboxThread.join();
    }
    {
        std::lock_guard<std::mutex> lock(g_chatboxQueueMutex);
        if (g_chatboxQueue) {
            g_runtime.setChatBoxSnapshot(g_chatboxQueue->snapshot());
            delete g_chatboxQueue;
            g_chatboxQueue = nullptr;
        }
    }
}

static void chatBoxQueueLoop() {
    while (g_chatboxThreadRunning) {
        {
            std::lock_guard<std::mutex> lock(g_chatboxQueueMutex);
            if (g_chatboxQueue) {
                g_chatboxQueue->tick();
                g_runtime.setChatBoxSnapshot(g_chatboxQueue->snapshot());
            }
        }
        Sleep(50);
    }
}

static void printRuntimeSnapshot(const char* label) {
    auto snapshot = g_runtime.snapshot();
    printf("[Runtime] %s running=%s phone=%s osc=%s typing=%s dry_run=%s sent_audio=%d queue_pending=%zu queue_sent=%zu\n",
           label,
           snapshot.running ? "true" : "false",
           snapshot.phone_connected ? "true" : "false",
           snapshot.osc_ready ? "true" : "false",
           snapshot.typing_active ? "true" : "false",
           snapshot.chatbox_dry_run ? "true" : "false",
           snapshot.sent_audio_count,
           snapshot.chatbox.pending_count,
           snapshot.chatbox.sent_count);
}

static void startStatusServer() {
    g_controller.setReconnectFn(reconnectPhone);
    g_statusServer.setControlHandler(handleControlRequest);
    if (!g_statusServer.start("127.0.0.1", 8766)) {
        g_runtime.setLastError("Status HTTP server failed to start");
    }
}

static void stopStatusServer() {
    g_statusServer.stop();
}

static std::string controlJson(bool ok, const std::string& action, const std::string& message) {
    return stt::toControlJson({ok, action, message});
}

static std::string urlDecode(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '+' ) {
            out.push_back(' ');
        } else if (value[i] == '%' && i + 2 < value.size() &&
                   std::isxdigit((unsigned char)value[i + 1]) &&
                   std::isxdigit((unsigned char)value[i + 2])) {
            char hex[3] = { value[i + 1], value[i + 2], 0 };
            out.push_back((char)std::strtol(hex, nullptr, 16));
            i += 2;
        } else {
            out.push_back(value[i]);
        }
    }
    return out;
}

static std::string extractControlValue(const std::string& path, const std::string& body, const std::string& key) {
    const std::string queryNeedle = key + "=";
    size_t queryStart = path.find('?');
    if (queryStart != std::string::npos) {
        std::string query = path.substr(queryStart + 1);
        size_t pos = query.find(queryNeedle);
        if (pos != std::string::npos) {
            pos += queryNeedle.size();
            size_t end = query.find('&', pos);
            return urlDecode(query.substr(pos, end == std::string::npos ? std::string::npos : end - pos));
        }
    }

    const std::string jsonNeedle = "\"" + key + "\"";
    size_t keyPos = body.find(jsonNeedle);
    if (keyPos == std::string::npos) return "";
    size_t colon = body.find(':', keyPos + jsonNeedle.size());
    if (colon == std::string::npos) return "";
    size_t quoteStart = body.find('"', colon + 1);
    if (quoteStart == std::string::npos) return "";
    size_t quoteEnd = body.find('"', quoteStart + 1);
    if (quoteEnd == std::string::npos) return "";
    return body.substr(quoteStart + 1, quoteEnd - quoteStart - 1);
}

static std::string selectedAudioDeviceName(const std::vector<stt::WasapiCapture::AudioInputDevice>& devices,
                                           const std::string& selectedId) {
    for (const auto& device : devices) {
        if (!selectedId.empty() && device.id == selectedId) return device.name;
    }
    for (const auto& device : devices) {
        if (device.is_default) return device.name;
    }
    return selectedId.empty() ? "Default recording device" : selectedId;
}

static void refreshAudioInputSnapshot(const std::string& mode) {
    auto devices = stt::WasapiCapture::listInputDevices();
    std::string selectedId;
    {
        std::lock_guard<std::mutex> lock(g_audioInputMutex);
        selectedId = g_selectedAudioDeviceId;
    }

    stt::PcAudioInputSnapshot snapshot;
    snapshot.mode = mode;
    snapshot.selected_device_id = selectedId;
    snapshot.selected_device_name = mode == "loopback" ? "Default playback loopback" : selectedAudioDeviceName(devices, selectedId);
    snapshot.devices.reserve(devices.size());
    for (const auto& device : devices) {
        snapshot.devices.push_back({device.id, device.name, device.is_default});
    }
    g_runtime.setAudioInputSnapshot(std::move(snapshot));
}

static bool setSelectedAudioDevice(const std::string& deviceId, bool requestRestart) {
    if (g_audioLoopbackMode) {
        g_runtime.setLastError("Audio device selection is unavailable in loopback mode");
        return false;
    }

    auto devices = stt::WasapiCapture::listInputDevices();
    bool found = deviceId.empty();
    for (const auto& device : devices) {
        if (device.id == deviceId) {
            found = true;
            break;
        }
    }
    if (!found) {
        g_runtime.setLastError("Audio input device not found");
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(g_audioInputMutex);
        g_selectedAudioDeviceId = deviceId;
    }
    refreshAudioInputSnapshot("capture");
    if (requestRestart) {
        g_restartWasapiCapture = true;
    }
    return true;
}

static int listAudioDevicesAndExit() {
    auto devices = stt::WasapiCapture::listInputDevices();
    printf("[Audio] Active Windows recording devices:\n");
    if (devices.empty()) {
        printf("  (none)\n");
        return 1;
    }
    for (const auto& device : devices) {
        printf("  %s%s\n", device.is_default ? "* " : "  ", device.name.c_str());
        printf("    id: %s\n", device.id.c_str());
    }
    return 0;
}

static std::string handleControlRequest(const std::string& path, const std::string& body) {
    const std::string basePath = path.substr(0, path.find('?'));
    if (basePath == "/control/listen/start") {
        return stt::toControlJson(g_controller.startListening());
    }

    if (basePath == "/control/listen/stop") {
        setTyping(false);
        return stt::toControlJson(g_controller.stopListening());
    }

    if (basePath == "/control/phone/reconnect") {
        return stt::toControlJson(g_controller.reconnectPhone());
    }

    if (basePath == "/control/audio/input-device") {
        std::string deviceId = extractControlValue(path, body, "id");
        bool ok = setSelectedAudioDevice(deviceId, true);
        return controlJson(ok, "audio.input-device", ok ? "audio input device selected" : "audio input device unavailable");
    }

    if (basePath == "/control/queue/clear") {
        std::lock_guard<std::mutex> lock(g_chatboxQueueMutex);
        if (!g_chatboxQueue) return controlJson(false, "queue.clear", "queue unavailable");
        g_chatboxQueue->clear();
        g_runtime.setChatBoxSnapshot(g_chatboxQueue->snapshot());
        return controlJson(true, "queue.clear", "queue cleared");
    }

    if (basePath == "/control/queue/pause" || basePath == "/control/queue/resume") {
        bool paused = basePath == "/control/queue/pause";
        std::lock_guard<std::mutex> lock(g_chatboxQueueMutex);
        if (!g_chatboxQueue) return controlJson(false, paused ? "queue.pause" : "queue.resume", "queue unavailable");
        g_chatboxQueue->setPaused(paused);
        g_runtime.setChatBoxSnapshot(g_chatboxQueue->snapshot());
        return controlJson(true, paused ? "queue.pause" : "queue.resume", paused ? "queue paused" : "queue resumed");
    }

    if (basePath == "/control/error/clear") {
        g_runtime.clearLastError();
        return controlJson(true, "error.clear", "error cleared");
    }

    if (basePath == "/control/chatbox/clear") {
        if (!g_oscReady || g_chatboxDryRun) {
            g_runtime.setLastError("ChatBox clear unavailable");
            return controlJson(false, "chatbox.clear", "chatbox clear unavailable");
        }
        g_oscSender.clearChatBox();
        return controlJson(true, "chatbox.clear", "chatbox cleared");
    }

    return controlJson(false, "unknown", "unknown control path");
}

static bool reconnectPhone() {
    g_networkClient.disconnect();
    g_runtime.setPhoneConnected(false);
    bool ok = g_networkClient.connect(g_config.network.host, g_config.network.port);
    if (ok) {
        g_networkClient.setTextCallback(onTextReceived);
    }
    return ok;
}

static std::string resolveVadModelPath() {
    try {
        auto path = stt::embeddedSileroVadPath();
        if (!path.empty()) {
            printf("[VAD] Using embedded model: %s\n", path.string().c_str());
            return path.string();
        }
    } catch (const std::exception& e) {
        printf("[VAD] Embedded model unavailable: %s\n", e.what());
    }
    return g_config.vad.model;
}

void setColor(int color) {
    SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), color);
}

void printHeader() {
    setColor(11);
    printf("\n");
    printf("============================================================\n");
    printf("       VRChat STT - Speech to ChatBox                      \n");
    printf("       SenseVoice-Large + QNN NPU                          \n");
    printf("============================================================\n");
    printf("\n");
    setColor(7);
}

BOOL WINAPI consoleHandler(DWORD fdwCtrlType) {
    if (fdwCtrlType == CTRL_C_EVENT || fdwCtrlType == CTRL_CLOSE_EVENT) {
        g_running = false;
        return TRUE;
    }
    return FALSE;
}

void onTextReceived(const stt::TextResult& result) {
    if (!result.success || result.text.empty()) return;
    setTyping(false);
    if (result.segment_id != 0) {
        printf("[Network] Text segment id: %u\n", result.segment_id);
    }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_lastText = result.text;
        g_receivedText = true;
    }
    g_runtime.setLastText(result.text, result.emotion);
    g_textCv.notify_all();
    
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_currentText.clear();
        g_currentEmotion = result.emotion;
    }

    sendChatBoxText(result.text, result.emotion);
}

static void setTyping(bool typing) {
    if (!g_oscReady || g_chatboxDryRun) return;
    if (g_typingActive == typing) return;
    if (g_oscSender.sendTyping(typing)) {
        g_typingActive = typing;
        g_runtime.setTypingActive(typing);
    }
}

static void sendChatBoxText(const std::string& text, const std::string& emotion) {
    std::string emoji = stt::emotionToEmoji(stt::parseEmotion(emotion));
    auto chunks = g_chatboxFormatter.splitText(text);
    std::vector<std::string> finalTexts;
    for (const auto& chunk : chunks) {
        if (!g_chatboxFormatter.shouldSend(chunk)) {
            printf("[ChatBox] Skipped duplicate: %s\n", chunk.c_str());
            continue;
        }
        std::string finalText = chunk;
        if (!emoji.empty() && g_config.display.show_emotion_icon) {
            finalText = emoji + " " + finalText;
        }
        if (g_chatboxDryRun) {
            printf("[ChatBox dry-run] %s\n", finalText.c_str());
            continue;
        }
        finalTexts.push_back(finalText);
    }

    if (!finalTexts.empty()) {
        std::lock_guard<std::mutex> lock(g_chatboxQueueMutex);
        if (g_chatboxQueue) {
            g_chatboxQueue->enqueue(finalTexts);
            g_runtime.setChatBoxSnapshot(g_chatboxQueue->snapshot());
        }
    }

    if (!emotion.empty() && emotion != "NEUTRAL") {
        printf("[ChatBox] Emotion: %s %s\n", emotion.c_str(), emoji.c_str());
    }
}

static uint16_t readU16LE(const std::vector<uint8_t>& data, size_t offset) {
    return (uint16_t)(data[offset] | (data[offset + 1] << 8));
}

static uint32_t readU32LE(const std::vector<uint8_t>& data, size_t offset) {
    return (uint32_t)data[offset] |
           ((uint32_t)data[offset + 1] << 8) |
           ((uint32_t)data[offset + 2] << 16) |
           ((uint32_t)data[offset + 3] << 24);
}

static bool readWav16Mono(const std::string& path, std::vector<float>& samples, int& sampleRate) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        printf("[WAV] Failed to open: %s\n", path.c_str());
        return false;
    }

    std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (data.size() < 44 || std::string((char*)data.data(), 4) != "RIFF" ||
        std::string((char*)data.data() + 8, 4) != "WAVE") {
        printf("[WAV] Not a WAV file: %s\n", path.c_str());
        return false;
    }

    uint16_t format = readU16LE(data, 20);
    uint16_t channels = readU16LE(data, 22);
    sampleRate = (int)readU32LE(data, 24);
    uint16_t bitsPerSample = readU16LE(data, 34);
    if (format != 1 || channels != 1 || bitsPerSample != 16) {
        printf("[WAV] Expected PCM16 mono, got format=%u channels=%u bits=%u\n",
               format, channels, bitsPerSample);
        return false;
    }

    size_t offset = 12;
    size_t dataOffset = 0;
    uint32_t dataLength = 0;
    while (offset + 8 <= data.size()) {
        std::string chunkId((char*)data.data() + offset, 4);
        uint32_t chunkLength = readU32LE(data, offset + 4);
        if (chunkId == "data") {
            dataOffset = offset + 8;
            dataLength = chunkLength;
            break;
        }
        offset += 8 + chunkLength + (chunkLength % 2);
    }

    if (dataOffset == 0 || dataOffset + dataLength > data.size()) {
        printf("[WAV] data chunk not found or invalid\n");
        return false;
    }

    size_t count = dataLength / 2;
    samples.resize(count);
    for (size_t i = 0; i < count; ++i) {
        int16_t sample = (int16_t)readU16LE(data, dataOffset + i * 2);
        samples[i] = sample / 32768.0f;
    }
    return true;
}

static int runWavMode(const std::string& wavPath) {
    std::vector<float> samples;
    int sampleRate = 0;
    if (!readWav16Mono(wavPath, samples, sampleRate)) {
        return 1;
    }
    if (sampleRate != 16000) {
        printf("[WAV] Expected 16000Hz, got %dHz\n", sampleRate);
        return 1;
    }

    printf("[WAV] Loaded %zu samples (%.2fs): %s\n",
           samples.size(), (float)samples.size() / sampleRate, wavPath.c_str());

    printf("[Network] Connecting to %s:%d...\n", g_config.network.host.c_str(), g_config.network.port);
    if (!g_networkClient.connect(g_config.network.host, g_config.network.port)) {
        printf("[Error] Failed to connect to phone\n");
        g_runtime.setPhoneConnected(false);
        g_runtime.setLastError("Failed to connect to phone");
        return 1;
    }
    g_runtime.setPhoneConnected(true);

    g_networkClient.setTextCallback(onTextReceived);
    auto sendStart = std::chrono::steady_clock::now();
    g_networkClient.sendAudio(samples.data(), samples.size(), true, g_nextSegmentId.fetch_add(1));
    printf("[Network] Sent WAV audio, waiting for text...\n");

    std::unique_lock<std::mutex> lock(g_mutex);
    bool gotText = g_textCv.wait_for(lock, std::chrono::seconds(30), [] { return g_receivedText; });
    std::string text = g_lastText;
    lock.unlock();

    g_networkClient.disconnect();
    g_runtime.setPhoneConnected(false);

    if (!gotText) {
        printf("[Error] Timed out waiting for text\n");
        return 1;
    }

    auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - sendStart
    ).count();
    printf("[Timing] PC send-to-text: %lld ms\n", (long long)elapsedMs);
    printf("[Text] %s\n", text.c_str());
    return 0;
}

static int runWavVadMode(const std::string& wavPath, bool simulateLivePadding) {
    std::vector<float> samples;
    int sampleRate = 0;
    if (!readWav16Mono(wavPath, samples, sampleRate)) {
        return 1;
    }
    if (sampleRate != 16000) {
        printf("[WAV] Expected 16000Hz, got %dHz\n", sampleRate);
        return 1;
    }

    printf("%s Loaded %zu samples (%.2fs): %s\n",
           simulateLivePadding ? "[SIMULATE-WAV-VAD]" : "[WAV-VAD]",
           samples.size(), (float)samples.size() / sampleRate, wavPath.c_str());

    g_preRollBuffer.setCapacitySeconds(kLiveRingBufferSeconds);

    std::string vadModelPath = resolveVadModelPath();
    if (!g_vad.init(vadModelPath, g_config.vad.silence_threshold)) {
        printf("[Error] Failed to initialize VAD\n");
        printf("  Model path: %s\n", vadModelPath.c_str());
        return 1;
    }

    if (!g_networkClient.connect(g_config.network.host, g_config.network.port)) {
        printf("[Error] Failed to connect to phone\n");
        g_runtime.setPhoneConnected(false);
        g_runtime.setLastError("Failed to connect to phone");
        return 1;
    }
    g_runtime.setPhoneConnected(true);

    g_networkClient.setTextCallback(onTextReceived);
    g_vad.setCallbacks(nullptr, onSpeechSegment);
    auto sendStart = std::chrono::steady_clock::now();

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_receivedText = false;
        g_lastText.clear();
        g_sentAudioCount = 0;
        g_vadSourceSamples = simulateLivePadding ? nullptr : &samples;
        g_applyVadPadding = true;
        g_preRollBuffer.clear();
    }

    const size_t chunkSize = 1600;
    for (size_t offset = 0; offset < samples.size(); offset += chunkSize) {
        size_t remaining = samples.size() - offset;
        size_t n = remaining < chunkSize ? remaining : chunkSize;
        if (simulateLivePadding) {
            g_preRollBuffer.append(samples.data() + offset, n);
        }
        g_vad.process(samples.data() + offset, n);
        if (simulateLivePadding) {
            flushPendingLiveSegments(false);
        }
    }
    g_vad.flush();
    flushPendingLiveSegments(true);

    if (g_buffer.hasData() && g_networkClient.isConnected()) {
        auto merged = g_buffer.getMergedAudio();
        g_networkClient.sendAudio(merged.data(), merged.size(), true, g_nextSegmentId.fetch_add(1));
        g_sentAudioCount++;
        g_runtime.incrementSentAudioCount();
        g_buffer.clear();
        printf("[Network] Sent final %.2fs buffered audio to phone\n", (float)merged.size() / 16000.0f);
    }

    bool sentAnyAudio = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        sentAnyAudio = g_sentAudioCount > 0;
    }
    if (!sentAnyAudio) {
        printf("[WAV-VAD] No speech segment was emitted by VAD for this file\n");
        printf("[WAV-VAD] Try a louder/longer speech WAV or use --wav to bypass VAD\n");
        g_networkClient.disconnect();
        g_runtime.setPhoneConnected(false);
        clearVadPaddingSource();
        return 1;
    }

    std::unique_lock<std::mutex> lock(g_mutex);
    bool gotText = g_textCv.wait_for(lock, std::chrono::seconds(30), [] { return g_receivedText; });
    std::string text = g_lastText;
    lock.unlock();

    g_networkClient.disconnect();
    g_runtime.setPhoneConnected(false);
    clearVadPaddingSource();

    if (!gotText) {
        printf("[Error] Timed out waiting for text\n");
        return 1;
    }

    auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - sendStart
    ).count();
    printf("[Timing] PC %s start-to-text: %lld ms\n",
           simulateLivePadding ? "simulate-wav-vad" : "wav-vad",
           (long long)elapsedMs);
    printf("[Text] %s\n", text.c_str());
    return 0;
}

void addPaddedSegmentToBuffer(const stt::SpeechSegment& segment, const float* samples, size_t numSamples,
                              float paddedStart, float paddedEnd) {
    const float segmentEnd = segment.start_time + segment.duration;

    printf("[VAD] Speech segment: %.2fs - %.2fs (%.2fs), padded %.2fs - %.2fs (%.2fs)\n",
           segment.start_time,
           segmentEnd,
           segment.duration,
           paddedStart,
           paddedStart + (float)numSamples / kSampleRate,
           (float)numSamples / kSampleRate);

    g_buffer.addSegment(samples, numSamples);
    
    if (g_buffer.shouldSend() && g_networkClient.isConnected()) {
        auto merged = g_buffer.getMergedAudio();
        g_networkClient.sendAudio(merged.data(), merged.size(), true, g_nextSegmentId.fetch_add(1));
        g_sentAudioCount++;
        g_runtime.incrementSentAudioCount();
        g_buffer.clear();
        
        printf("[Network] Sent %.2fs audio to phone\n", (float)merged.size() / 16000.0f);
    }
}

void flushPendingLiveSegments(bool force) {
    while (true) {
        PendingLiveSegment pending;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_pendingLiveSegments.empty()) return;
            const auto& front = g_pendingLiveSegments.front();
            if (!force && g_preRollBuffer.currentTimeSeconds() < front.paddedEnd) return;
            pending = front;
            g_pendingLiveSegments.pop_front();
        }

        const float segmentEnd = pending.segment.start_time + pending.segment.duration;
        auto postRoll = g_preRollBuffer.getRange(segmentEnd, pending.paddedEnd);
        std::vector<float> paddedSamples;
        paddedSamples.reserve(pending.preRoll.size() + pending.segment.samples.size() + postRoll.size());
        paddedSamples.insert(paddedSamples.end(), pending.preRoll.begin(), pending.preRoll.end());
        paddedSamples.insert(paddedSamples.end(), pending.segment.samples.begin(), pending.segment.samples.end());
        paddedSamples.insert(paddedSamples.end(), postRoll.begin(), postRoll.end());
        if (!paddedSamples.empty()) {
            addPaddedSegmentToBuffer(pending.segment, paddedSamples.data(), paddedSamples.size(),
                                     pending.paddedStart, pending.paddedEnd);
        }
    }
}

void onSpeechSegment(const stt::SpeechSegment& segment) {
    setTyping(true);
    const float segmentEnd = segment.start_time + segment.duration;
    const float paddedStart = segment.start_time > kVadPreRollSeconds ? segment.start_time - kVadPreRollSeconds : 0.0f;
    const float paddedEnd = segmentEnd + kVadPostRollSeconds;

    std::vector<float> paddedSamples;
    const float* segmentData = segment.samples.data();
    size_t segmentSize = segment.samples.size();
    bool deferLiveSegment = false;

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_applyVadPadding && g_vadSourceSamples) {
            size_t startSample = (size_t)(paddedStart * kSampleRate);
            size_t endSample = (size_t)(paddedEnd * kSampleRate);
            if (startSample > g_vadSourceSamples->size()) startSample = g_vadSourceSamples->size();
            if (endSample > g_vadSourceSamples->size()) endSample = g_vadSourceSamples->size();
            if (endSample > startSample) {
                paddedSamples.assign(g_vadSourceSamples->begin() + startSample, g_vadSourceSamples->begin() + endSample);
                segmentData = paddedSamples.data();
                segmentSize = paddedSamples.size();
            }
        } else if (g_applyVadPadding) {
            auto preRoll = g_preRollBuffer.getRange(paddedStart, segment.start_time);
            PendingLiveSegment pending;
            pending.segment = segment;
            pending.preRoll = std::move(preRoll);
            pending.paddedStart = paddedStart;
            pending.paddedEnd = paddedEnd;
            g_pendingLiveSegments.push_back(std::move(pending));
            deferLiveSegment = true;
        }
    }

    if (deferLiveSegment) {
        return;
    }

    addPaddedSegmentToBuffer(segment, segmentData, segmentSize, paddedStart, paddedEnd);
}

void simulateAudioInput() {
    printf("[Simulate] Running in simulation mode (no VRChat OSC)\n");
    printf("[Simulate] Reading from default microphone...\n\n");
    
    const int sampleRate = 16000;
    const int chunkSize = sampleRate / 10;
    std::vector<float> samples(chunkSize);
    
    while (g_running) {
        if (!g_runtime.snapshot().listening_active) {
            Sleep(100);
            continue;
        }

        for (int i = 0; i < chunkSize; i++) {
            samples[i] = 0.0f;
        }
        
        static float phase = 0.0f;
        static bool generating = false;
        
        if (GetAsyncKeyState(VK_SPACE) & 0x8000) {
            generating = true;
            for (int i = 0; i < chunkSize; i++) {
                samples[i] = 0.3f * sinf(phase);
                phase += 2.0f * 3.14159f * 440.0f / sampleRate;
            }
        } else {
            generating = false;
            phase = 0.0f;
        }
        
        g_preRollBuffer.append(samples.data(), chunkSize);
        g_vad.process(samples.data(), chunkSize);
        flushPendingLiveSegments(false);
        
        Sleep(100);
    }
}

static void processLiveSamples(const float* samples, size_t sampleCount) {
    if (!samples || sampleCount == 0) return;
    if (!g_runtime.snapshot().listening_active) return;
    g_preRollBuffer.append(samples, sampleCount);
    g_vad.process(samples, sampleCount);
    flushPendingLiveSegments(false);
}

static bool runWasapiAudioInput(bool loopback) {
    g_audioLoopbackMode = loopback;
    const auto mode = loopback ? stt::WasapiCapture::Mode::DefaultOutputLoopback
                               : stt::WasapiCapture::Mode::DefaultInput;
    refreshAudioInputSnapshot(loopback ? "loopback" : "capture");
    printf("[Audio] Starting Windows %s through WASAPI...\n", loopback ? "output loopback" : "input");

    bool lastRunOk = true;
    while (g_running) {
        std::string selectedId;
        {
            std::lock_guard<std::mutex> lock(g_audioInputMutex);
            selectedId = g_selectedAudioDeviceId;
        }

        if (!g_wasapiCapture.start(processLiveSamples, mode, loopback ? "" : selectedId)) {
            g_runtime.setLastError("Failed to start WASAPI capture");
            return false;
        }

        bool restartRequested = false;
        while (g_running && g_wasapiCapture.isRunning()) {
            Sleep(100);
            if (g_restartWasapiCapture.exchange(false)) {
                printf("[Audio] Restarting WASAPI capture with selected input device\n");
                restartRequested = true;
                g_wasapiCapture.stop();
                break;
            }

            auto error = g_wasapiCapture.lastError();
            if (!error.empty()) {
                printf("[Audio] %s\n", error.c_str());
                g_runtime.setLastError(error);
                lastRunOk = false;
                g_wasapiCapture.stop();
                return false;
            }
        }

        g_wasapiCapture.stop();
        refreshAudioInputSnapshot(loopback ? "loopback" : "capture");
        if (!g_running) break;
        if (!restartRequested) {
            break;
        }
    }

    return lastRunOk;
}

int main(int argc, char* argv[]) {
    SetConsoleOutputCP(65001);
    SetConsoleCP(65001);
    setvbuf(stdout, NULL, _IONBF, 0);  // unbuffered output for real-time console
    
    printHeader();
    
    g_config = stt::loadConfig("config.json");
    
    printf("[Config] Loading configuration...\n");
    printf("  VAD threshold: %.2f\n", g_config.vad.silence_threshold);
    printf("  Merge window: %.2fs\n", g_config.merge.window);
    printf("  Network: %s:%d\n", g_config.network.host.c_str(), g_config.network.port);
    printf("\n");

    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--chatbox-dry-run") {
            g_chatboxDryRun = true;
        }
    }
    bool useSimulationInput = false;
    bool useAudioLoopback = false;
    bool listAudioDevices = false;
    std::string selectedAudioDeviceId;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--simulate-input") {
            useSimulationInput = true;
        }
        if (arg == "--audio-loopback") {
            useAudioLoopback = true;
        }
        if (arg == "--list-audio-devices") {
            listAudioDevices = true;
        }
        if (arg == "--audio-device-id" && i + 1 < argc) {
            selectedAudioDeviceId = argv[++i];
        }
    }

    if (listAudioDevices) {
        return listAudioDevicesAndExit();
    }

    if (!selectedAudioDeviceId.empty()) {
        if (useAudioLoopback) {
            printf("[Audio] --audio-device-id is ignored with --audio-loopback\n");
        } else if (!setSelectedAudioDevice(selectedAudioDeviceId, false)) {
            printf("[Audio] Selected device id was not found: %s\n", selectedAudioDeviceId.c_str());
            return 1;
        }
    }

    g_runtime.setRunning(true);
    g_runtime.setChatBoxDryRun(g_chatboxDryRun);
    g_runtime.setListeningActive(true);

    if (argc >= 3 && std::string(argv[1]) == "--wav") {
        return runWavMode(argv[2]);
    }

    if (argc >= 3 && std::string(argv[1]) == "--wav-vad") {
        return runWavVadMode(argv[2], false);
    }

    if (argc >= 3 && std::string(argv[1]) == "--simulate-wav-vad") {
        return runWavVadMode(argv[2], true);
    }
    startStatusServer();
    
    std::string vadModelPath = resolveVadModelPath();
    if (!g_vad.init(vadModelPath, g_config.vad.silence_threshold)) {
        printf("[Error] Failed to initialize VAD\n");
        printf("  Model path: %s\n", vadModelPath.c_str());
        printf("  Please ensure the model file exists\n");
        return 1;
    }
    
    g_vad.setCallbacks(nullptr, onSpeechSegment);
    g_preRollBuffer.setCapacitySeconds(kLiveRingBufferSeconds);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_applyVadPadding = true;
        g_vadSourceSamples = nullptr;
        g_preRollBuffer.clear();
    }
    
    g_chatboxFormatter.setMaxChars((size_t)g_config.display.max_text_length);

    if (!g_oscSender.init(g_config.osc.send.ip, g_config.osc.send.port)) {
        printf("[Warning] OSC sender initialization failed\n");
        g_oscReady = false;
    } else {
        g_oscReady = true;
    }
    g_runtime.setOscReady(g_oscReady);
    startChatBoxQueue();
    
    printf("[Network] Connecting to phone...\n");
    printf("  Make sure the phone app is running\n");
    printf("  Run: adb forward tcp:%d tcp:%d\n", g_config.network.port, g_config.network.port);
    printf("\n");
    
    if (!g_networkClient.connect(g_config.network.host, g_config.network.port)) {
        printf("[Error] Failed to connect to phone\n");
        g_runtime.setPhoneConnected(false);
        g_runtime.setLastError("Failed to connect to phone");
        printf("  Check USB connection and run:\n");
        printf("  adb forward tcp:%d tcp:%d\n", g_config.network.port, g_config.network.port);
        printf("\n  Make sure the phone app is running (press Start)\n");
        printf("  Press Enter to retry, or Ctrl+C to quit...\n");
        getchar();
        printf("[Network] Retrying connection...\n");
        if (!g_networkClient.connect(g_config.network.host, g_config.network.port)) {
            printf("[Error] Still failed. Press Enter to exit...\n");
            g_runtime.setPhoneConnected(false);
            g_runtime.setLastError("Failed to connect to phone after retry");
            stopChatBoxQueue();
            stopStatusServer();
            getchar();
            return 1;
        }
    }
    g_runtime.setPhoneConnected(true);
    
    g_networkClient.setTextCallback(onTextReceived);
    
    SetConsoleCtrlHandler(consoleHandler, TRUE);
    
    printf("============================================================\n");
    setColor(10);
    printf(" Ready! Speak into your microphone\n");
    setColor(7);
    printf(" Press [Q] or [Ctrl+C] to quit\n");
    if (useSimulationInput) {
        printf(" Simulation input enabled: hold [Space] to generate tone\n");
    } else if (useAudioLoopback) {
        printf(" Audio loopback enabled: Windows playback is used as input\n");
    }
    printf("============================================================\n\n");
    printRuntimeSnapshot("ready");
    
    if (useSimulationInput) {
        simulateAudioInput();
    } else {
        runWasapiAudioInput(useAudioLoopback);
    }
    
    printf("\n[Shutdown] Stopping...\n");
    
    g_vad.flush();
    setTyping(false);
    stopChatBoxQueue();
    stopStatusServer();
    g_networkClient.disconnect();
    g_runtime.setPhoneConnected(false);
    g_oscSender.clearChatBox();
    g_runtime.setListeningActive(false);
    g_runtime.setRunning(false);
    printRuntimeSnapshot("shutdown");
    
    setColor(10);
    printf("[Shutdown] Complete\n");
    setColor(7);
    
    return 0;
}
