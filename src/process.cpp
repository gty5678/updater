#include "process.hpp"
#include "utils.hpp"

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <chrono>
#include <sstream>
#include <string>
#include <thread>

namespace updater {

bool waitForPidExit(uint32_t pid, int timeoutSeconds) {
    if (pid == 0) return true;

    auto end = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSeconds);

    while (std::chrono::steady_clock::now() < end) {
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!h) {
            return true;
        }
        DWORD exitCode = 0;
        if (GetExitCodeProcess(h, &exitCode)) {
            CloseHandle(h);
            if (exitCode != STILL_ACTIVE) {
                return true;
            }
        } else {
            CloseHandle(h);
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    return false;
}

static std::vector<std::string> splitArgs(const std::string& s) {
    std::vector<std::string> result;
    std::istringstream iss(s);
    std::string part;
    while (iss >> part) {
        result.push_back(part);
    }
    return result;
}

static std::wstring utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) return L"";
    int size = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (size <= 0) return L"";
    std::wstring result(size, 0);
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &result[0], size);
    result.resize(size - 1);
    return result;
}

bool startMainExe(const std::string& exePath,
                  const std::string& workingDir,
                  const std::string& mainArgs) {
    std::wstring exePathW = utf8ToWide(exePath);
    std::wstring cmdLineW = L"\"" + exePathW + L"\"";
    if (!mainArgs.empty()) {
        std::wstring mainArgsW = utf8ToWide(mainArgs);
        cmdLineW += L" " + mainArgsW;
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    std::vector<wchar_t> cmdLineBuf(cmdLineW.begin(), cmdLineW.end());
    cmdLineBuf.push_back(L'\0');

    BOOL ok = CreateProcessW(
        exePathW.c_str(),
        cmdLineBuf.data(),
        nullptr,
        nullptr,
        FALSE,
        0,
        nullptr,
        workingDir.empty() ? nullptr : utf8ToWide(workingDir).c_str(),
        &si,
        &pi
    );

    if (ok) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return true;
    }
    return false;
}

}  // namespace updater
#endif
