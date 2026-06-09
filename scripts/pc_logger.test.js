const assert = require("assert");
const fs = require("fs");
const path = require("path");

const root = path.resolve(__dirname, "..");

const loggerHeaderPath = path.join(root, "src/pc/pc_logger.h");
const loggerSourcePath = path.join(root, "src/pc/pc_logger.cpp");
const runtimeHeaderPath = path.join(root, "src/pc/pc_runtime.h");
const statusJsonPath = path.join(root, "src/pc/pc_status_json.cpp");
const statusPagePath = path.join(root, "src/pc/pc_status_page.cpp");
const cmakePath = path.join(root, "src/pc/CMakeLists.txt");
const buildPcPath = path.join(root, "scripts/build_pc.bat");
const verifyReleasePath = path.join(root, "scripts/verify_release.bat");

assert(fs.existsSync(loggerHeaderPath), "PC logger should have a focused header");
assert(fs.existsSync(loggerSourcePath), "PC logger should have a focused implementation");

const header = fs.readFileSync(loggerHeaderPath, "utf8");
const source = fs.readFileSync(loggerSourcePath, "utf8");
const runtimeHeader = fs.readFileSync(runtimeHeaderPath, "utf8");
const statusJson = fs.readFileSync(statusJsonPath, "utf8");
const statusPage = fs.readFileSync(statusPagePath, "utf8");
const cmake = fs.readFileSync(cmakePath, "utf8");
const buildPc = fs.readFileSync(buildPcPath, "utf8");
const verifyRelease = fs.readFileSync(verifyReleasePath, "utf8");

assert(header.includes("enum class PcLogLevel"), "PC logger should expose log levels");
assert(header.includes("struct PcLogEntry"), "PC logger should expose recent log entries");
assert(header.includes("class PcLogger"), "PC logger should expose a logger class");
assert(header.includes("configureDefaultFileSink"), "PC logger should support default file logging");
assert(header.includes("recentEntries"), "PC logger should expose a bounded recent log ring");
assert(header.includes("logf("), "PC logger should support printf-style migration");

assert(source.includes("LOCALAPPDATA"), "PC logger should put normal logs under LOCALAPPDATA");
assert(source.includes("PocketVoice") && source.includes("logs") && source.includes("pc.log"), "PC logger should use the PocketVoice log folder and pc.log");
assert(source.includes("OutputDebugStringA"), "PC logger should still be visible to debugger tools");
assert(source.includes("std::lock_guard<std::mutex>"), "PC logger should be thread-safe");
assert(source.includes("m_recent.pop_front()"), "PC logger should bound the in-memory log list");
assert(source.includes("std::filesystem::create_directories"), "PC logger should create its log directory");

assert(runtimeHeader.includes("std::vector<PcLogEntry> recent_logs"), "PC runtime snapshot should expose recent logs");
assert(statusJson.includes("\\\"recent_logs\\\""), "Status JSON should include recent_logs");
assert(statusJson.includes("\\\"category\\\"") && statusJson.includes("\\\"message\\\""), "Status JSON should serialize structured log fields");
assert(statusPage.includes("id=\"event-log\""), "PC UI should include an event log surface");
assert(statusPage.includes("function renderEventLog"), "PC UI should render recent log entries");
assert(statusPage.includes("data.recent_logs"), "PC UI should consume recent_logs from /status");

assert(cmake.includes("pc_logger.cpp") && cmake.includes("pc_logger.h"), "CMake build should include the PC logger");
assert(buildPc.includes("pc_logger.cpp"), "Direct MSVC build should compile the PC logger");
assert(verifyRelease.includes("pc_logger.test.js"), "Release static tests should include PC logger coverage");

console.log("pc_logger tests passed");
