#include "pc_logger.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <sstream>
#include <utility>

namespace stt {
namespace {

int64_t nowUnixMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::string timestampText(int64_t timestampMs) {
    std::time_t seconds = static_cast<std::time_t>(timestampMs / 1000);
    std::tm localTime{};
    localtime_s(&localTime, &seconds);

    char buffer[64] = {};
    std::snprintf(
        buffer,
        sizeof(buffer),
        "%04d-%02d-%02d %02d:%02d:%02d.%03lld",
        localTime.tm_year + 1900,
        localTime.tm_mon + 1,
        localTime.tm_mday,
        localTime.tm_hour,
        localTime.tm_min,
        localTime.tm_sec,
        static_cast<long long>(timestampMs % 1000));
    return buffer;
}

std::filesystem::path defaultLogPath() {
    char* localAppData = nullptr;
    size_t envSize = 0;
    std::filesystem::path base;
    if (_dupenv_s(&localAppData, &envSize, "LOCALAPPDATA") == 0 && localAppData) {
        base = localAppData;
        std::free(localAppData);
    } else {
        base = std::filesystem::temp_directory_path();
    }
    return base / "PocketVoice" / "logs" / "pc.log";
}

std::string formatLine(const PcLogEntry& entry) {
    std::ostringstream out;
    out << timestampText(entry.timestamp_ms)
        << " [" << pcLogLevelText(entry.level) << "]"
        << " [" << entry.category << "] "
        << entry.message;
    return out.str();
}

std::string vformat(const char* format, va_list args) {
    if (!format) return "";

    va_list copy;
    va_copy(copy, args);
    int count = std::vsnprintf(nullptr, 0, format, copy);
    va_end(copy);
    if (count <= 0) return "";

    std::string out(static_cast<size_t>(count) + 1, '\0');
    std::vsnprintf(out.data(), out.size(), format, args);
    out.resize(static_cast<size_t>(count));
    return out;
}

}  // namespace

const char* pcLogLevelText(PcLogLevel level) {
    switch (level) {
        case PcLogLevel::Info: return "info";
        case PcLogLevel::Warning: return "warn";
        case PcLogLevel::Error: return "error";
    }
    return "info";
}

void PcLogger::configureDefaultFileSink() {
    configureFileSink(defaultLogPath());
}

void PcLogger::configureFileSink(std::filesystem::path filePath) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_filePath = std::move(filePath);
    if (!m_filePath.empty()) {
        std::filesystem::create_directories(m_filePath.parent_path());
    }
}

void PcLogger::setRecentLimit(size_t limit) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_recentLimit = limit > 0 ? limit : 1;
    while (m_recent.size() > m_recentLimit) {
        m_recent.pop_front();
    }
}

void PcLogger::log(PcLogLevel level, std::string category, std::string message) {
    PcLogEntry entry;
    entry.timestamp_ms = nowUnixMs();
    entry.level = level;
    entry.category = std::move(category);
    entry.message = std::move(message);

    const std::string line = formatLine(entry);
    std::lock_guard<std::mutex> lock(m_mutex);
    m_recent.push_back(entry);
    while (m_recent.size() > m_recentLimit) {
        m_recent.pop_front();
    }
    writeLocked(entry, line);
}

void PcLogger::logf(PcLogLevel level, const char* category, const char* format, ...) {
    va_list args;
    va_start(args, format);
    std::string message = vformat(format, args);
    va_end(args);
    log(level, category ? category : "General", std::move(message));
}

std::vector<PcLogEntry> PcLogger::recentEntries() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return std::vector<PcLogEntry>(m_recent.begin(), m_recent.end());
}

std::filesystem::path PcLogger::filePath() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_filePath;
}

void PcLogger::writeLocked(const PcLogEntry& entry, const std::string& line) {
    (void)entry;
    std::string output = line + "\n";
    std::fputs(output.c_str(), stdout);
    OutputDebugStringA(output.c_str());

    if (m_filePath.empty()) return;
    std::ofstream file(m_filePath, std::ios::app | std::ios::binary);
    if (file.is_open()) {
        file << output;
    }
}

PcLogger& pcLogger() {
    static PcLogger logger;
    return logger;
}

void pcLog(PcLogLevel level, const char* category, std::string message) {
    pcLogger().log(level, category ? category : "General", std::move(message));
}

void pcLogf(PcLogLevel level, const char* category, const char* format, ...) {
    va_list args;
    va_start(args, format);
    std::string message = vformat(format, args);
    va_end(args);
    pcLogger().log(level, category ? category : "General", std::move(message));
}

}  // namespace stt
