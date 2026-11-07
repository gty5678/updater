#include "process.hpp"

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>

namespace updater {

bool waitForPidExit(uint32_t pid, int timeoutSeconds) {
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!process) return GetLastError() == ERROR_INVALID_PARAMETER;
    const auto result = WaitForSingleObject(process, static_cast<DWORD>(timeoutSeconds) * 1000);
    CloseHandle(process);
    return result == WAIT_OBJECT_0;
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
    std::wstring workDirW = utf8ToWide(workingDir);
    std::wstring mainArgsW = utf8ToWide(mainArgs);

    // 使用 Shell 的「打开」启动，与当前控制台进程脱钩，效果类似资源管理器双击
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = L"open";
    sei.lpFile = exePathW.c_str();
    sei.lpDirectory = workDirW.empty() ? nullptr : workDirW.c_str();
    sei.lpParameters = mainArgs.empty() ? nullptr : mainArgsW.c_str();
    sei.nShow = SW_SHOWNORMAL;

    return ShellExecuteExW(&sei) != FALSE;
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

}  // namespace updater

#else
#include <cerrno>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fcntl.h>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace updater {

std::string getCurrentExePath() {
#ifdef __APPLE__
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> path(size);
    if (_NSGetExecutablePath(path.data(), &size) == 0) return path.data();
    throw std::runtime_error("Cannot locate updater executable");
#else
    return std::filesystem::read_symlink("/proc/self/exe").u8string();
#endif
}

bool waitForPidExit(uint32_t pid, int timeoutSeconds) {
    if (pid > static_cast<uint32_t>(std::numeric_limits<pid_t>::max())) return false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSeconds);
    do {
        if (kill(static_cast<pid_t>(pid), 0) == -1 && errno == ESRCH) return true;
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    } while (true);
}

bool startMainExe(const std::string& exePath, const std::string& workingDir,
                  const std::string& mainArgs) {
    std::vector<std::string> args{exePath};
    std::istringstream input(mainArgs);
    while (input >> std::ws && !input.eof()) {
        std::string arg;
        if (!(input >> std::quoted(arg))) return false;
        args.push_back(std::move(arg));
    }
    std::vector<char*> argv;
    for (auto& arg : args) argv.push_back(arg.data());
    argv.push_back(nullptr);

    int statusPipe[2];
    if (pipe(statusPipe) != 0) return false;
    if (fcntl(statusPipe[1], F_SETFD, FD_CLOEXEC) == -1) {
        close(statusPipe[0]);
        close(statusPipe[1]);
        return false;
    }
    const pid_t child = fork();
    if (child == 0) {
        close(statusPipe[0]);
        if (setsid() != -1 && chdir(workingDir.c_str()) == 0) {
            execv(exePath.c_str(), argv.data());
        }
        const char failure = 1;
        while (write(statusPipe[1], &failure, sizeof(failure)) == -1 && errno == EINTR) {}
        _exit(127);
    }
    close(statusPipe[1]);
    char failure;
    ssize_t count = -1;
    if (child > 0) {
        do {
            count = read(statusPipe[0], &failure, sizeof(failure));
        } while (count == -1 && errno == EINTR);
        if (count > 0) {
            while (waitpid(child, nullptr, 0) == -1 && errno == EINTR) {}
        }
    }
    close(statusPipe[0]);
    return child > 0 && count == 0;
}

}  // namespace updater
#endif
