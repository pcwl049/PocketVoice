#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "wasapi_capture.h"

#include "sample_converter.h"

#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>
#include <windows.h>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace stt {

namespace {

template <typename T>
static void releaseCom(T*& ptr) {
    if (ptr) {
        ptr->Release();
        ptr = nullptr;
    }
}

static std::string hresultText(const char* action, HRESULT hr) {
    char buffer[128];
    std::snprintf(buffer, sizeof(buffer), "%s failed: 0x%08lx", action, (unsigned long)hr);
    return buffer;
}

static AudioSampleType sampleTypeFromWaveFormat(const WAVEFORMATEX* format) {
    if (!format) return AudioSampleType::Float32;
    if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return AudioSampleType::Float32;
    if (format->wFormatTag == WAVE_FORMAT_PCM) return AudioSampleType::Pcm16;
    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
        if (extensible->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) return AudioSampleType::Float32;
        if (extensible->SubFormat == KSDATAFORMAT_SUBTYPE_PCM) return AudioSampleType::Pcm16;
    }
    return AudioSampleType::Float32;
}

static size_t bytesPerSample(AudioSampleType type) {
    return type == AudioSampleType::Pcm16 ? sizeof(int16_t) : sizeof(float);
}

static std::string wideToUtf8(const wchar_t* value) {
    if (!value || !*value) return "";
    int bytes = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 1) return "";
    std::string out((size_t)bytes - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, -1, out.data(), bytes, nullptr, nullptr);
    return out;
}

static std::string deviceId(IMMDevice* device) {
    if (!device) return "";
    LPWSTR id = nullptr;
    std::string result;
    if (SUCCEEDED(device->GetId(&id))) {
        result = wideToUtf8(id);
    }
    if (id) CoTaskMemFree(id);
    return result;
}

static std::string deviceFriendlyName(IMMDevice* device) {
    if (!device) return "";
    IPropertyStore* store = nullptr;
    PROPVARIANT value;
    PropVariantInit(&value);
    std::string result;
    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store)) &&
        SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &value)) &&
        value.vt == VT_LPWSTR) {
        result = wideToUtf8(value.pwszVal);
    }
    PropVariantClear(&value);
    releaseCom(store);
    return result;
}

static std::wstring utf8ToWide(const std::string& value) {
    if (value.empty()) return L"";
    int chars = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    if (chars <= 1) return L"";
    std::wstring out((size_t)chars - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, out.data(), chars);
    return out;
}

}  // namespace

WasapiCapture::~WasapiCapture() {
    stop();
}

bool WasapiCapture::start(SamplesCallback callback, Mode mode) {
    return start(std::move(callback), mode, "");
}

bool WasapiCapture::start(SamplesCallback callback, Mode mode, std::string deviceId) {
    if (m_running || !callback) return false;
    setLastError("");
    m_running = true;
    m_thread = std::thread(&WasapiCapture::run, this, std::move(callback), mode, std::move(deviceId));
    return true;
}

void WasapiCapture::stop() {
    m_running = false;
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

bool WasapiCapture::isRunning() const {
    return m_running;
}

std::string WasapiCapture::lastError() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_lastError;
}

void WasapiCapture::setLastError(const std::string& error) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_lastError = error;
}

std::vector<WasapiCapture::AudioInputDevice> WasapiCapture::listInputDevices() {
    std::vector<AudioInputDevice> devices;
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool comInitialized = SUCCEEDED(hr);
    if (!comInitialized) {
        return devices;
    }

    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDeviceCollection* collection = nullptr;
    IMMDevice* defaultDevice = nullptr;
    std::string defaultId;

    do {
        hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                              __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator));
        if (FAILED(hr)) break;

        if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &defaultDevice))) {
            defaultId = deviceId(defaultDevice);
        }

        hr = enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection);
        if (FAILED(hr)) break;

        UINT count = 0;
        if (FAILED(collection->GetCount(&count))) break;

        for (UINT i = 0; i < count; ++i) {
            IMMDevice* device = nullptr;
            if (FAILED(collection->Item(i, &device))) continue;

            AudioInputDevice info;
            info.id = deviceId(device);
            info.name = deviceFriendlyName(device);
            if (info.name.empty()) info.name = "Audio input";
            info.is_default = !defaultId.empty() && info.id == defaultId;
            devices.push_back(std::move(info));

            releaseCom(device);
        }
    } while (false);

    releaseCom(defaultDevice);
    releaseCom(collection);
    releaseCom(enumerator);
    CoUninitialize();
    return devices;
}

void WasapiCapture::run(SamplesCallback callback, Mode mode, std::string selectedDeviceId) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool comInitialized = SUCCEEDED(hr);
    if (!comInitialized) {
        setLastError(hresultText("CoInitializeEx", hr));
        m_running = false;
        return;
    }

    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* audioClient = nullptr;
    IAudioCaptureClient* captureClient = nullptr;
    WAVEFORMATEX* mixFormat = nullptr;
    HANDLE samplesReady = nullptr;
    HANDLE avrtHandle = nullptr;
    DWORD avrtTaskIndex = 0;

    do {
        hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                              __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator));
        if (FAILED(hr)) {
            setLastError(hresultText("CoCreateInstance(MMDeviceEnumerator)", hr));
            break;
        }

        if (mode == Mode::DefaultInput && !selectedDeviceId.empty()) {
            std::wstring wideId = utf8ToWide(selectedDeviceId);
            hr = enumerator->GetDevice(wideId.c_str(), &device);
            if (FAILED(hr)) {
                setLastError(hresultText("GetDevice", hr));
                break;
            }
        } else {
            const EDataFlow dataFlow = mode == Mode::DefaultOutputLoopback ? eRender : eCapture;
            hr = enumerator->GetDefaultAudioEndpoint(dataFlow, eConsole, &device);
            if (FAILED(hr)) {
                setLastError(hresultText("GetDefaultAudioEndpoint", hr));
                break;
            }
        }

        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&audioClient));
        if (FAILED(hr)) {
            setLastError(hresultText("Activate(IAudioClient)", hr));
            break;
        }

        hr = audioClient->GetMixFormat(&mixFormat);
        if (FAILED(hr)) {
            setLastError(hresultText("GetMixFormat", hr));
            break;
        }

        samplesReady = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!samplesReady) {
            setLastError("CreateEventW failed");
            break;
        }

        DWORD streamFlags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
        if (mode == Mode::DefaultOutputLoopback) {
            streamFlags |= AUDCLNT_STREAMFLAGS_LOOPBACK;
        }

        hr = audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                     streamFlags,
                                     10000000,
                                     0,
                                     mixFormat,
                                     nullptr);
        if (FAILED(hr)) {
            setLastError(hresultText("IAudioClient::Initialize", hr));
            break;
        }

        hr = audioClient->SetEventHandle(samplesReady);
        if (FAILED(hr)) {
            setLastError(hresultText("SetEventHandle", hr));
            break;
        }

        hr = audioClient->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&captureClient));
        if (FAILED(hr)) {
            setLastError(hresultText("GetService(IAudioCaptureClient)", hr));
            break;
        }

        avrtHandle = AvSetMmThreadCharacteristicsW(L"Audio", &avrtTaskIndex);

        const AudioFormat sourceFormat{
            (int)mixFormat->nSamplesPerSec,
            (int)mixFormat->nChannels,
            sampleTypeFromWaveFormat(mixFormat),
        };
        const size_t sourceFrameBytes = bytesPerSample(sourceFormat.sample_type) * (size_t)sourceFormat.channels;

        std::string captureName = deviceFriendlyName(device);
        std::printf("[Audio] WASAPI %s started: %s, %d Hz, %d channel(s)\n",
                    mode == Mode::DefaultOutputLoopback ? "loopback" : "capture",
                    captureName.empty() ? "default device" : captureName.c_str(),
                    sourceFormat.sample_rate,
                    sourceFormat.channels);

        hr = audioClient->Start();
        if (FAILED(hr)) {
            setLastError(hresultText("IAudioClient::Start", hr));
            break;
        }

        while (m_running) {
            DWORD waitResult = WaitForSingleObject(samplesReady, 200);
            if (waitResult != WAIT_OBJECT_0 && waitResult != WAIT_TIMEOUT) {
                setLastError("WaitForSingleObject failed");
                break;
            }

            UINT32 packetFrames = 0;
            hr = captureClient->GetNextPacketSize(&packetFrames);
            if (FAILED(hr)) {
                setLastError(hresultText("GetNextPacketSize", hr));
                break;
            }

            while (packetFrames > 0) {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                hr = captureClient->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
                if (FAILED(hr)) {
                    setLastError(hresultText("GetBuffer", hr));
                    break;
                }

                std::vector<uint8_t> silence;
                const uint8_t* source = data;
                size_t byteCount = (size_t)frames * sourceFrameBytes;
                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    silence.assign(byteCount, 0);
                    source = silence.data();
                }

                auto samples = convertTo16kMonoFloat(source, byteCount, sourceFormat);
                if (!samples.empty()) {
                    callback(samples.data(), samples.size());
                }

                hr = captureClient->ReleaseBuffer(frames);
                if (FAILED(hr)) {
                    setLastError(hresultText("ReleaseBuffer", hr));
                    break;
                }

                hr = captureClient->GetNextPacketSize(&packetFrames);
                if (FAILED(hr)) {
                    setLastError(hresultText("GetNextPacketSize", hr));
                    break;
                }
            }

            if (FAILED(hr)) break;
        }

        audioClient->Stop();
    } while (false);

    if (avrtHandle) AvRevertMmThreadCharacteristics(avrtHandle);
    if (samplesReady) CloseHandle(samplesReady);
    if (mixFormat) CoTaskMemFree(mixFormat);
    releaseCom(captureClient);
    releaseCom(audioClient);
    releaseCom(device);
    releaseCom(enumerator);
    CoUninitialize();
    m_running = false;
}

}  // namespace stt
