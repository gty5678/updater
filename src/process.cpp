#include "process.hpp"
#include "utils.hpp"

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <chrono>
#include <filesystem>
#include <fstream>
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

static std::string wideToUtf8(const std::wstring& wide) {
    if (wide.empty()) return "";
    int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0) return "";
    std::string result(size, 0);
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, &result[0], size, nullptr, nullptr);
    result.resize(size - 1);
    return result;
}

std::string getCurrentExePath() {
    wchar_t buf[MAX_PATH + 1] = {};
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return "";
    return wideToUtf8(buf);
}

bool copySelfAndLaunch(const std::string& destPath,
                       const std::string& workingDir,
                       int argc, char* argv[]) {
    std::string selfPath = getCurrentExePath();
    if (selfPath.empty()) return false;

    std::filesystem::path dest = std::filesystem::u8path(destPath);
    std::filesystem::path self = std::filesystem::u8path(selfPath);
    std::filesystem::create_directories(dest.parent_path());

    try {
        std::filesystem::copy_file(self, dest, std::filesystem::copy_options::overwrite_existing);
    } catch (...) {
        return false;
    }

    std::wstring cmdLineW = L"\"" + utf8ToWide(destPath) + L"\"";
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--running-from-copy") {
            if (i + 1 < argc) ++i;
            continue;
        }
        cmdLineW += L" ";
        bool needsQuote = (arg.find(' ') != std::string::npos || arg.find('"') != std::string::npos);
        if (needsQuote) cmdLineW += L"\"";
        cmdLineW += utf8ToWide(arg);
        if (needsQuote) cmdLineW += L"\"";
    }
    cmdLineW += L" --running-from-copy \"" + utf8ToWide(destPath) + L"\"";

    std::vector<wchar_t> cmdLineBuf(cmdLineW.begin(), cmdLineW.end());
    cmdLineBuf.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    std::wstring workDirW = utf8ToWide(workingDir);
    BOOL ok = CreateProcessW(
        utf8ToWide(destPath).c_str(),
        cmdLineBuf.data(),
        nullptr,
        nullptr,
        FALSE,
        0,
        nullptr,
        workDirW.empty() ? nullptr : workDirW.c_str(),
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

void scheduleSelfDelete(const std::string& exePath) {
    DWORD pid = GetCurrentProcessId();
    std::wstring exePathW = utf8ToWide(exePath);
    wchar_t tempPath[MAX_PATH];
    if (GetTempPathW(MAX_PATH, tempPath) == 0) return;
    std::wstring batchPath = tempPath;
    batchPath += L"DarkEyeUpdater_del_";
    batchPath += std::to_wstring(pid);
    batchPath += L".bat";

    std::wofstream batch(batchPath);
    if (!batch) return;
    batch << L"@echo off\n";
    batch << L":loop\n";
    batch << L"tasklist /fi \"PID eq " << pid << L"\" 2>nul | find \"" << pid << L"\" >nul\n";
    batch << L"if not errorlevel 1 (timeout /t 1 >nul & goto loop)\n";
    batch << L"del /f /q \"" << exePathW << L"\"\n";
    batch << L"del /f /q \"%~f0\"\n";
    batch.close();

    std::wstring cmdLine = L"cmd /c \"" + batchPath + L"\"";
    std::vector<wchar_t> cmdBuf(cmdLine.begin(), cmdLine.end());
    cmdBuf.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    if (CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, tempPath, &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
}

}  // namespace updater
#endif
