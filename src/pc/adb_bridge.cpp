#include "adb_bridge.h"
#include "pc_logger.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace stt {
namespace {

std::filesystem::path executableDir() {
    wchar_t buffer[MAX_PATH] = {};
    DWORD len = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return {};
    return std::filesystem::path(buffer).parent_path();
}

std::filesystem::path findAdbPath() {
    const auto exeDir = executableDir();
    const std::filesystem::path bundled = exeDir / "adb" / "adb.exe";
    if (std::filesystem::exists(bundled)) return bundled;

    const std::filesystem::path local = exeDir / "adb.exe";
    if (std::filesystem::exists(local)) return local;

    char* pathEnv = nullptr;
    size_t pathEnvSize = 0;
    if (_dupenv_s(&pathEnv, &pathEnvSize, "PATH") != 0 || !pathEnv) return {};

    std::stringstream paths(pathEnv);
    free(pathEnv);
    std::string item;
    while (std::getline(paths, item, ';')) {
        if (item.empty()) continue;
        std::filesystem::path candidate = std::filesystem::path(item) / "adb.exe";
        if (std::filesystem::exists(candidate)) return candidate;
    }
    return {};
}

std::string quote(const std::filesystem::path& path) {
    return "\"" + path.string() + "\"";
}

std::filesystem::path tempOutputPath(const char* name) {
    wchar_t tempDir[MAX_PATH] = {};
    DWORD len = GetTempPathW(MAX_PATH, tempDir);
    std::filesystem::path base = (len > 0 && len < MAX_PATH) ? std::filesystem::path(tempDir) : std::filesystem::temp_directory_path();
    return base / name;
}

int runAdbCapture(const std::filesystem::path& adb, const std::string& args, std::string& output) {
    const auto outPath = tempOutputPath("pocketvoice-adb.out.txt");
    std::filesystem::remove(outPath);

    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;

    HANDLE outFile = CreateFileW(
        outPath.wstring().c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ,
        &security,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_TEMPORARY,
        nullptr);
    if (outFile == INVALID_HANDLE_VALUE) {
        output = "Failed to create adb output file.";
        return 1;
    }

    std::wstring command = L"\"" + adb.wstring() + L"\" " + std::wstring(args.begin(), args.end());
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    startup.hStdOutput = outFile;
    startup.hStdError = outFile;

    PROCESS_INFORMATION process{};
    BOOL created = CreateProcessW(
        nullptr,
        command.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &startup,
        &process);

    CloseHandle(outFile);
    if (!created) {
        output = "Failed to start adb.";
        return 1;
    }

    DWORD wait = WaitForSingleObject(process.hProcess, 10000);
    if (wait == WAIT_TIMEOUT) {
        TerminateProcess(process.hProcess, 1);
        output = "ADB command timed out.";
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return 1;
    }
    DWORD exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);

    std::ifstream in(outPath, std::ios::binary);
    output = in.is_open() ? std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()) : "";
    return static_cast<int>(exitCode);
}

bool containsDeviceLine(const std::string& devicesOutput) {
    std::istringstream in(devicesOutput);
    std::string line;
    while (std::getline(in, line)) {
        if (line.find("\tdevice") != std::string::npos) return true;
    }
    return false;
}

} // namespace

AdbForwardResult ensureAdbForward(int port) {
    AdbForwardResult result;
    const auto adb = findAdbPath();
    if (adb.empty()) {
        result.message = "ADB not found. Put adb.exe in the bundled adb folder.";
        return result;
    }

    result.adb_found = true;
    result.adb_path = adb.string();

    std::string output;
    runAdbCapture(adb, "start-server", output);

    output.clear();
    int devicesCode = runAdbCapture(adb, "devices", output);
    result.unauthorized = output.find("unauthorized") != std::string::npos;
    result.device_found = containsDeviceLine(output);
    if (devicesCode != 0 || !result.device_found) {
        result.message = result.unauthorized ? "Android device is waiting for USB debugging authorization." :
                                               "No authorized Android device found over USB.";
        pcLogf(PcLogLevel::Error, "ADB", "%s\n%s", result.message.c_str(), output.c_str());
        return result;
    }

    output.clear();
    std::string forwardArgs = "forward tcp:" + std::to_string(port) + " tcp:" + std::to_string(port);
    int forwardCode = runAdbCapture(adb, forwardArgs, output);
    if (forwardCode != 0) {
        result.message = "ADB port forward failed: " + output;
        pcLog(PcLogLevel::Error, "ADB", result.message);
        return result;
    }

    result.ok = true;
    result.message = "ADB port forward active.";
    pcLogf(PcLogLevel::Info, "ADB", "%s tcp:%d -> tcp:%d", result.message.c_str(), port, port);
    return result;
}

}
