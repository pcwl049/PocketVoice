#pragma once

#include <cstdint>
#include <cstddef>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace stt {

enum class PcLogLevel {
    Info,
    Warning,
    Error,
};

struct PcLogEntry {
    int64_t timestamp_ms = 0;
    PcLogLevel level = PcLogLevel::Info;
    std::string category;
    std::string message;
};

class PcLogger {
public:
    void configureDefaultFileSink();
    void configureFileSink(std::filesystem::path filePath);
    void setRecentLimit(size_t limit);
    void log(PcLogLevel level, std::string category, std::string message);
    void logf(PcLogLevel level, const char* category, const char* format, ...);
    std::vector<PcLogEntry> recentEntries() const;
    std::filesystem::path filePath() const;

private:
    void writeLocked(const PcLogEntry& entry, const std::string& line);

    mutable std::mutex m_mutex;
    std::filesystem::path m_filePath;
    std::deque<PcLogEntry> m_recent;
    size_t m_recentLimit = 80;
};

PcLogger& pcLogger();

const char* pcLogLevelText(PcLogLevel level);
void pcLog(PcLogLevel level, const char* category, std::string message);
void pcLogf(PcLogLevel level, const char* category, const char* format, ...);

}  // namespace stt
