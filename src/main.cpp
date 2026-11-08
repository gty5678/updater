/**
 * 通用更新器主程序
 * 负责检查更新、下载安装包、解压覆盖、备份与回滚
 */
#include "download.hpp"
#include "fs_ops.hpp"
#include "json_parse.hpp"
#include "process.hpp"
#include "tarzst_extract.hpp"
#include "utils.hpp"
#include "zip_extract.hpp"
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#endif
#include <chrono>
#include <iostream>
#include <set>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef __APPLE__
#include <cerrno>
#include <cstdlib>
#include <sys/stat.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#ifdef __APPLE__
// Keep updating in the original process so callers still receive its exit code.
// Terminal only displays the log. Retain logs so failures remain diagnosable.
class UpdateTerminal {
    std::filesystem::path directory;
    bool launched = false;
    int result = 1;
public:
    void setResult(int code) { result = code; }
    const std::filesystem::path& path() const { return directory; }
    static bool reopen(const std::filesystem::path& lock) {
        if (const auto quiet = std::getenv("UPDATER_NO_CONSOLE"); quiet && std::string(quiet) == "1") return false;
        std::ifstream owner(lock / "console");
        long pid = 0;
        std::string directory;
        if (!(owner >> pid) || pid <= 0) return false;
        owner.ignore();
        std::getline(owner, directory);
        if (kill(static_cast<pid_t>(pid), 0) != 0) return false;
        const auto script = std::filesystem::path(directory) / "正在更新.command";
        if (!std::filesystem::is_regular_file(script)) return false;
        const auto child = fork();
        if (child == 0) {
            execl("/usr/bin/open", "open", "-b", "com.apple.Terminal", script.c_str(), nullptr);
            _exit(127);
        }
        int status = 0;
        pid_t waited;
        do { waited = child > 0 ? waitpid(child, &status, 0) : -1; }
        while (waited < 0 && errno == EINTR);
        return waited > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    }
    void show() {
        if (const auto quiet = std::getenv("UPDATER_NO_CONSOLE"); quiet && std::string(quiet) == "1") return;
        if (isatty(STDOUT_FILENO)) {
            std::cout << "\033]0;Updater - 正在更新，请勿关闭窗口\007";
            return;
        }
        auto pattern = (std::filesystem::temp_directory_path() / "updater-console-XXXXXX").string();
        if (!mkdtemp(pattern.data())) throw std::runtime_error("Cannot create update console directory");
        directory = pattern;
        const auto log = directory / "update.log";
        const auto script = directory / "正在更新.command";
        std::ofstream(log).close();
        // mkdtemp's path is generated locally, but quote it for shell metacharacters.
        auto quote = [](const std::string& value) {
            std::string result = "'";
            for (char c : value) result += c == '\'' ? "'\\''" : std::string(1, c);
            return result + "'";
        };
        std::ofstream command(script);
        command << "#!/bin/sh\ncd " << quote(directory.string()) << " || exit 1\n"
                << "printf '\\033]0;Updater - 正在更新，请勿关闭窗口\\007'\n"
                << "tail -n +1 -f update.log &\nviewer=$!\n"
                << "trap 'kill \"$viewer\" 2>/dev/null' EXIT\n"
                << "while [ ! -f done ]; do sleep 1; done\n"
                << "sleep 1\nkill \"$viewer\" 2>/dev/null\nwait \"$viewer\" 2>/dev/null\n"
                << "trap - EXIT\n"
                << "code=$(cat done)\n"
                << "case \"$code\" in\n"
                << "0) printf '\\n更新流程已完成。\\n' ;;\n"
                << "12) printf '\\n已有更新或安装锁，请查看上方说明。\\n' ;;\n"
                << "20) printf '\\n更新失败，已恢复原版本。\\n' ;;\n"
                << "*) printf '\\n更新未完成（退出码 %s），请查看上方错误和日志。\\n' \"$code\" ;;\nesac\n"
                << "printf '更新日志：%s\\n' " << quote(log.string()) << "\n";
        command.close();
        if (!command || chmod(script.c_str(), 0700) != 0)
            throw std::runtime_error("Cannot prepare update console");
        const auto child = fork();
        if (child == 0) {
            execl("/usr/bin/open", "open", "-b", "com.apple.Terminal", script.c_str(), nullptr);
            _exit(127);
        }
        int status = 0;
        pid_t waited;
        do { waited = child > 0 ? waitpid(child, &status, 0) : -1; }
        while (waited == -1 && errno == EINTR);
        if (waited < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
            throw std::runtime_error("Cannot open update terminal");
        launched = true;
        if (!std::freopen(log.c_str(), "a", stdout) || !std::freopen(log.c_str(), "a", stderr))
            throw std::runtime_error("Cannot open update console log");
        setvbuf(stdout, nullptr, _IONBF, 0);
        setvbuf(stderr, nullptr, _IONBF, 0);
    }
    ~UpdateTerminal() {
        if (!directory.empty()) {
            if (!launched) {
                std::error_code ignored;
                std::filesystem::remove_all(directory, ignored);
                return;
            }
            std::cout.flush();
            std::cerr.flush();
            std::ofstream(directory / "done") << result << '\n';
        }
    }
};
#endif

namespace {

/// 命令行参数与配置
struct Args {
    std::string latestJsonUrl;  ///< latest.json 的 URL
    std::string installDir;                         ///< 应用安装目录
    std::string mainExe;                  ///< 主程序可执行文件名
    std::string currentVersion;                           ///< 当前本地版本号
    std::vector<std::string> keep;             ///< 更新时保留的目录/文件列表
    bool rollbackOnFailure = true;                        ///< 更新失败时是否回滚
    uint32_t pid = 0;                                     ///< 主进程 PID，用于等待其退出
    int waitTimeoutSeconds = 120;                        ///< 等待主进程退出的超时（秒）
    int downloadTimeoutSeconds = 30;                      ///< 下载 latest.json 的超时
    int downloadRetries = 3;                              ///< 下载重试次数
    int packageTimeoutSeconds = 120;                      ///< 下载安装包的超时
    std::string mainArgs;                                 ///< 启动主程序时传递的参数
    bool startMain = true;                                 ///< 更新完成后是否自动启动主程序
    std::string updaterDir = ".updater";                   ///< 更新器工作目录
    std::string backupDirName = ".updater_backup";         ///< 备份根目录名
    std::string payloadRoot;
    std::string localPackage;                              ///< 非空则使用本地安装包，跳过远程与校验
};

/// 根据 URL 或文件路径（可含 ?query）判断安装包扩展名：.tar.zst 或 .zip
/// 先检查 .tar.zst（多级扩展名，rfind('.') 会误判为 .zst）
std::string packageExtFromUrlOrPath(const std::string& urlOrPath) {
    std::string s = urlOrPath;
    size_t q = s.find('?');
    if (q != std::string::npos) s.resize(q);
    if (s.size() >= 8 && s.compare(s.size() - 8, 8, ".tar.zst") == 0) {
        return ".tar.zst";
    }
    if (s.size() >= 4 && s.compare(s.size() - 4, 4, ".zip") == 0) {
        return ".zip";
    }
    return ".zip";  // 默认 zip
}

/// 解析 --keep 参数，支持逗号分隔，返回去重后的目录名集合
std::set<std::string> parseKeep(const std::vector<std::string>& keepList) {
    std::set<std::string> result;
    for (const auto& raw : keepList) {
        std::istringstream iss(raw);
        std::string part;
        while (std::getline(iss, part, ',')) {
            std::string name = updater::trim(part);
            while (!name.empty() && (name.back() == '/' || name.back() == '\\')) name.pop_back();
            if (!name.empty()) {
                result.insert(name);
            }
        }
    }
    return result;
}

/// 解析命令行参数，填充 args
void parseArgs(int argc, char* argv[], Args& args) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--latest-json-url" && i + 1 < argc) {
            args.latestJsonUrl = argv[++i];
        } else if (arg == "--install-dir" && i + 1 < argc) {
            args.installDir = argv[++i];
        } else if (arg == "--main-exe" && i + 1 < argc) {
            args.mainExe = argv[++i];
        } else if (arg == "--current-version" && i + 1 < argc) {
            args.currentVersion = argv[++i];
        } else if (arg == "--keep" && i + 1 < argc) {
            args.keep.push_back(argv[++i]);
        } else if (arg == "--rollback-on-failure") {
            args.rollbackOnFailure = true;
        } else if (arg == "--no-rollback-on-failure") {
            args.rollbackOnFailure = false;
        } else if (arg == "--pid" && i + 1 < argc) {
            const std::string value = argv[++i];
            if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos ||
                std::stoull(value) > UINT32_MAX) {
                throw std::runtime_error("Invalid PID: " + value);
            }
            args.pid = static_cast<uint32_t>(std::stoull(value));
        } else if (arg == "--wait-timeout-seconds" && i + 1 < argc) {
            args.waitTimeoutSeconds = std::stoi(argv[++i]);
        } else if (arg == "--download-timeout-seconds" && i + 1 < argc) {
            args.downloadTimeoutSeconds = std::stoi(argv[++i]);
        } else if (arg == "--download-retries" && i + 1 < argc) {
            args.downloadRetries = std::stoi(argv[++i]);
        } else if (arg == "--package-timeout-seconds" && i + 1 < argc) {
            args.packageTimeoutSeconds = std::stoi(argv[++i]);
        } else if (arg == "--main-args" && i + 1 < argc) {
            args.mainArgs = argv[++i];
        } else if (arg == "--start-main") {
            args.startMain = true;
        } else if (arg == "--no-start-main") {
            args.startMain = false;
        } else if (arg == "--updater-dir" && i + 1 < argc) {
            args.updaterDir = argv[++i];
        } else if (arg == "--backup-dir-name" && i + 1 < argc) {
            args.backupDirName = argv[++i];
        } else if (arg == "--payload-root" && i + 1 < argc) {
            args.payloadRoot = argv[++i];
        } else if (arg == "--local-package" && i + 1 < argc) {
            args.localPackage = argv[++i];
        } else {
            throw std::runtime_error("Unknown option or missing value: " + arg);
        }
    }
}

/// 生成当前时间戳字符串，格式：YYYYMMDD_HHMMSS
std::string getTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf;
#ifdef _WIN32
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%Y%m%d_%H%M%S");
    return oss.str();
}

void printHelp() {
    std::cout << "updater --install-dir DIR [--main-exe PATH | --no-start-main]\n"
                 "        (--latest-json-url URL --current-version VERSION | --local-package FILE)\n"
                 "        [--keep NAME[,NAME...]] [--pid PID] [--main-args ARGS] [--payload-root PATH]\n"
                 "        [--wait-timeout-seconds N] [--download-timeout-seconds N]\n"
                 "        [--package-timeout-seconds N] [--download-retries N]\n"
                 "        [--updater-dir NAME] [--backup-dir-name NAME] [--no-rollback-on-failure]\n";
}

}  // namespace

#ifdef _WIN32
static void waitForConsoleClose(int exitCode) {
    if (const auto quiet = std::getenv("UPDATER_NO_PAUSE"); quiet && std::string(quiet) == "1") return;
    updater::log("更新流程已结束，退出码：" + std::to_string(exitCode));
    updater::log("请查看上方过程信息。按 Enter 键关闭窗口。");
    // stdin may be a pipe or NUL when launched by a desktop application.
    // Read the visible console directly, rather than returning immediately on EOF.
    const auto input = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  nullptr, OPEN_EXISTING, 0, nullptr);
    if (input != INVALID_HANDLE_VALUE) {
        FlushConsoleInputBuffer(input);
        INPUT_RECORD event{};
        DWORD count = 0;
        while (ReadConsoleInputW(input, &event, 1, &count)) {
            if (count && event.EventType == KEY_EVENT && event.Event.KeyEvent.bKeyDown &&
                event.Event.KeyEvent.wVirtualKeyCode == VK_RETURN) break;
        }
        CloseHandle(input);
    } else {
        MessageBoxW(nullptr, L"更新流程已结束，请查看更新日志。点击确定关闭。",
                    L"Updater", MB_OK | (exitCode ? MB_ICONERROR : MB_ICONINFORMATION));
    }
}

static void showUpdateConsole() {
    // A caller may launch us with CREATE_NO_WINDOW or a hidden console.
    // Allocate a console if needed, then reconnect output for progress/logs.
    if (!GetConsoleWindow()) {
        const auto redirected = [](DWORD stream) {
            const auto type = GetFileType(GetStdHandle(stream));
            return type == FILE_TYPE_PIPE || type == FILE_TYPE_DISK;
        };
        const bool keepStdout = redirected(STD_OUTPUT_HANDLE);
        const bool keepStderr = redirected(STD_ERROR_HANDLE);
        // CREATE_NO_WINDOW can attach us to a windowless console. AllocConsole
        // fails while still attached, so detach before creating a visible one.
        FreeConsole();
        if (AllocConsole()) {
            // Preserve explicit pipe/file redirection used by callers.
            if (!keepStdout) std::freopen("CONOUT$", "w", stdout);
            if (!keepStderr) std::freopen("CONOUT$", "w", stderr);
            std::cout.clear();
            std::cerr.clear();
        }
    }
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    SetConsoleTitleW(L"Updater - 正在更新，请勿关闭窗口");
    if (const auto window = GetConsoleWindow()) {
        // The first ShowWindow may obey the launcher's STARTF_USESHOWWINDOW
        // (including SW_HIDE) instead of our argument. Restore on a second call
        // so hidden and minimized launches both become visible.
        ShowWindow(window, SW_SHOW);
        ShowWindow(window, SW_RESTORE);
        if (!SetForegroundWindow(window)) {
            // Windows may deny focus to a background process. Let the user
            // find the updater through its taskbar button in that case.
            FLASHWINFO flash{sizeof(FLASHWINFO), window,
                             FLASHW_TRAY | FLASHW_TIMERNOFG, 3, 0};
            FlashWindowEx(&flash);
        }
    }
}

/// Windows 下 argv 为系统 ANSI，与内部 UTF-8 路径不一致；从 Unicode 命令行解析为 UTF-8
static std::string wideToUtf8MainArgv(const wchar_t* wide) {
    if (!wide || !*wide) return "";
    int size = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0) return "";
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, result.data(), size, nullptr, nullptr);
    result.pop_back();
    return result;
}

struct Utf8ArgvHolder {
    std::vector<std::string> storage;
    std::vector<char*> argv;
};

static bool replaceArgvWithUtf8FromCommandLine(Utf8ArgvHolder& out) {
    int wargc = 0;
    wchar_t** wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (!wargv) return false;
    out.storage.clear();
    out.argv.clear();
    out.storage.reserve(static_cast<size_t>(wargc));
    for (int i = 0; i < wargc; ++i) {
        out.storage.push_back(wideToUtf8MainArgv(wargv[i]));
    }
    LocalFree(wargv);
    for (auto& s : out.storage) {
        out.argv.push_back(s.data());
    }
    return true;
}
#endif

int main(int argc, char* argv[]) {
    // 退出码：0=成功/无需更新, 1=致命错误, 2=package_url 为空, 3=SHA256 校验失败,
    //        10=缺少 --current-version, 11=本地安装包路径无效, 12=安装目录已锁定,
    //        20=更新失败已回滚, 21=回滚失败, 22=更新失败未回滚
    int exitCode = 0;
#ifdef _WIN32
    Utf8ArgvHolder utf8Argv;
    if (replaceArgvWithUtf8FromCommandLine(utf8Argv)) {
        argc = static_cast<int>(utf8Argv.argv.size());
        argv = utf8Argv.argv.data();
    }
#endif

#ifdef _WIN32
    // 设置控制台为 UTF-8，确保中文日志正常显示
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif

    if (argc == 1 || (argc == 2 && std::string(argv[1]) == "--help")) {
#ifdef _WIN32
        if (argc == 1) showUpdateConsole();
#endif
        printHelp();
#ifdef _WIN32
        if (argc == 1) waitForConsoleClose(0);
#endif
        return 0;
    }

#ifdef _WIN32
    showUpdateConsole();
#endif
#ifdef __APPLE__
    UpdateTerminal terminal;
#endif
    std::string lockPathToCleanup;
    std::string updaterDirPathToCleanup;
    std::string backupRootPathToCleanup;

    try {
        Args args;
        parseArgs(argc, argv, args);

        namespace fs = std::filesystem;
        if (args.installDir.empty()) {
            throw std::runtime_error("--install-dir is required");
        }
        if (args.localPackage.empty() && args.latestJsonUrl.empty()) {
            throw std::runtime_error("--latest-json-url or --local-package is required");
        }

        const auto appDir = fs::canonical(fs::u8path(args.installDir));
        if (!fs::is_directory(appDir) || appDir == appDir.root_path()) {
            throw std::runtime_error("Install directory must be an existing application directory");
        }

        const auto self = fs::canonical(fs::u8path(updater::getCurrentExePath()));
        for (auto parent = self.parent_path(); !parent.empty() && parent != parent.root_path();
             parent = parent.parent_path()) {
            if (fs::equivalent(parent, appDir)) {
                throw std::runtime_error("Run updater from outside the installation directory");
            }
        }

        const auto workName = updater::safeRelativePath(fs::u8path(args.updaterDir));
        const auto backupName = updater::safeRelativePath(fs::u8path(args.backupDirName));
        if (workName == "." || backupName == "." || workName.has_parent_path() ||
            backupName.has_parent_path() || workName == backupName) {
            throw std::runtime_error("Work and backup directories must be distinct top-level names");
        }
        args.updaterDir = workName.u8string();
        args.backupDirName = backupName.u8string();
        const auto updaterDir = appDir / workName;
        const auto backupRoot = appDir / backupName;
        const auto mainPath = args.mainExe.empty() ? fs::path{} :
            updater::safeRelativePath(fs::u8path(args.mainExe));
        if (args.startMain && (mainPath.empty() || mainPath == ".")) {
            throw std::runtime_error("--main-exe or --no-start-main is required");
        }
        if (args.waitTimeoutSeconds < 0 || args.waitTimeoutSeconds > 86400) {
            throw std::runtime_error("--wait-timeout-seconds must be between 0 and 86400");
        }
        auto keepUser = parseKeep(args.keep);
        for (const auto& name : keepUser) {
            if (name == "." || fs::u8path(name).has_parent_path() ||
                updater::safeRelativePath(fs::u8path(name)) != fs::u8path(name)) {
                throw std::runtime_error("--keep expects top-level names");
            }
        }

        const auto lock = appDir / ".update-lock";
        if (!fs::create_directory(lock)) {
#ifdef __APPLE__
            if (UpdateTerminal::reopen(lock)) {
                updater::log("更新正在进行，已打开当前任务的进度窗口，请勿重复更新。");
                return 12;
            }
            terminal.show();
#endif
            updater::log("安装目录已锁定：可能已有更新正在运行，或上次更新异常中断。请先检查原进度窗口；确认更新器退出后再检查备份和遗留锁。");
            updater::log("锁目录：" + lock.u8string());
#ifdef __APPLE__
            terminal.setResult(12);
#endif
            exitCode = 12;
            goto exit_main;
        }
        lockPathToCleanup = lock.u8string();
#ifdef __APPLE__
        terminal.show();
        if (!terminal.path().empty()) {
            std::ofstream owner(lock / "console");
            owner << getpid() << '\n' << terminal.path().string() << '\n';
        }
#endif
        updater::log("更新正在进行，请等待完成。关闭进度窗口不会取消后台更新，请勿重复启动。");

        if (!fs::create_directory(updaterDir)) {
            throw std::runtime_error("Updater work directory already exists: " + updaterDir.u8string());
        }
        updaterDirPathToCleanup = updaterDir.u8string();

        updater::log("Working in: " + appDir.u8string());

        std::filesystem::path packagePath;
        std::string packageExt;

        if (!args.localPackage.empty()) {
            // 使用本地安装包：不下载 latest.json，不比较版本，不校验 SHA256
            packagePath =
                std::filesystem::absolute(std::filesystem::u8path(args.localPackage));
            if (!std::filesystem::is_regular_file(packagePath)) {
                updater::log("Local package not found or not a file: " + packagePath.u8string());
                exitCode = 11;
                goto exit_main;
            }
            // 根据路径扩展名决定格式：.tar.zst 或 .zip（规则与 package_url 一致）
            packageExt = packageExtFromUrlOrPath(packagePath.u8string());
            updater::log("Local package mode: " + packagePath.u8string());
            updater::log("Skipping remote version check and SHA256 verification.");
        } else {
            // 校验必须的 --current-version 参数
            std::string localVersion = updater::trim(args.currentVersion);
            if (localVersion.empty()) {
                updater::log("Missing --current-version. Example: --current-version 1.1.1");
                exitCode = 10;
                goto exit_main;
            }
            updater::log("Local version: " + localVersion);

            // 下载 latest.json 并解析远程版本信息
            auto rawJson = updater::downloadBytes(args.latestJsonUrl,
                                                  args.downloadTimeoutSeconds,
                                                  args.downloadRetries);
            std::string jsonStr(rawJson.begin(), rawJson.end());
            auto latest = updater::loadLatestInfo(jsonStr, UPDATER_PLATFORM);
            updater::log("Remote version: " + latest.latestVersion);

            // 比较版本号，若远程不比本地新则无需更新
            if (!updater::isNewer(latest.latestVersion, localVersion)) {
                updater::log("No update needed.");
                goto exit_main;
            }

            updater::log("Update is available. Preparing package download...");

            // 检查安装包 URL 是否有效
            if (latest.packageUrl.empty()) {
                updater::log("package_url is empty.");
                exitCode = 2;
                goto exit_main;
            }


            // 根据 package_url 扩展名决定格式：.tar.zst 或 .zip
            packageExt = packageExtFromUrlOrPath(latest.packageUrl);
            packagePath = fs::u8path(updater::downloadCachePath(appDir.u8string(), latest.packageUrl,
                                                              latest.packageSha256));
            updater::log("Package download cache (retained on interruption): " + packagePath.u8string());

            // 下载更新包，带进度条
            updater::downloadFile(
                latest.packageUrl,
                packagePath.u8string(),
                latest.packageSha256,
                args.packageTimeoutSeconds,
                args.downloadRetries,
                [](size_t downloaded, size_t total) {
                    double pct = (total > 0) ? (100.0 * downloaded / total) : 0.0;
                    updater::printProgressBar("Download", pct, downloaded, total, false);
                });
            updater::progressBarEnd();
            updater::log("Downloaded update package: " + packagePath.u8string());

            // 若提供了 SHA256，则校验下载文件的完整性
            if (!updater::isPlaceholderSha256(latest.packageSha256)) {
                std::string got = updater::sha256File(packagePath.u8string());
                std::string exp = latest.packageSha256;
                for (auto& c : got) {
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                }
                for (auto& c : exp) {
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                }
                if (got != exp) {
                    updater::log("SHA256 mismatch. expected=" + exp + " got=" + got);
                    fs::remove(packagePath);
                    fs::remove(fs::u8path(packagePath.u8string() + ".json"));
                    exitCode = 3;
                    goto exit_main;
                }
                updater::log("SHA256 verified.");
            } else {
                updater::log("SHA256 not provided or placeholder; skipping verification.");
            }
            const auto stagedPackage = updaterDir / ("update" + packageExt);
            fs::copy_file(packagePath, stagedPackage);
            fs::remove(fs::u8path(packagePath.u8string() + ".json"));
            fs::remove(packagePath);
            packagePath = stagedPackage;
        }

        // 等待主进程退出，以便安全覆盖文件
        if (args.pid > 0) {
            updater::log("Waiting for main PID " + std::to_string(args.pid) + " to exit...");
            bool ok = updater::waitForPidExit(args.pid, args.waitTimeoutSeconds);
            if (!ok) {
                throw std::runtime_error("Application is still running after timeout");
            }
        }

        // 创建带时间戳的备份目录并备份当前应用
        if (!fs::create_directory(backupRoot)) {
            throw std::runtime_error("Backup directory already exists: " + backupRoot.u8string());
        }
        backupRootPathToCleanup = backupRoot.u8string();
        std::string ts = getTimestamp();
        std::filesystem::path backupDir = backupRoot / ("backup_" + ts);
        updater::log("Backing up app to: " + backupDir.u8string());

        updater::ensureEmptyOrCreate(backupDir.u8string());
        updater::backupAppDir(appDir.u8string(), backupDir.u8string(), args.updaterDir, args.backupDirName);

        // 更新时保留的目录（含 .updater 和 .updater_backup）
        auto keepRelUpdate = keepUser;
        keepRelUpdate.insert(args.updaterDir);
        keepRelUpdate.insert(args.backupDirName);
        keepRelUpdate.insert(".update-lock");
        auto keepRelRollback = keepRelUpdate;

        bool installationStarted = false;
        try {
            // 解压到 .updater/unpacked（zip 或 tar.zst 根据格式自动选择）
            updater::log("Unpacking package...");
            std::filesystem::path unpackedDir = updaterDir / "unpacked";
            updater::ensureEmptyOrCreate(unpackedDir.u8string());

            auto progressFn = [](size_t current, size_t total) {
                double pct = (total > 0) ? (100.0 * current / total) : 0.0;
                updater::printProgressBar("Extract", pct, current, total, true);
            };
            if (packageExt == ".tar.zst") {
                updater::extractTarZstToDir(packagePath.u8string(), unpackedDir.u8string(), progressFn);
            } else {
                updater::extractZipToDir(packagePath.u8string(), unpackedDir.u8string(), progressFn);
            }
            updater::progressBarEnd();

            // 定位解压后的 payload 根目录（可能有一层包装目录）
            auto payloadRoot = unpackedDir;
            if (!args.payloadRoot.empty()) {
                payloadRoot /= updater::safeRelativePath(fs::u8path(args.payloadRoot));
            } else if (!args.startMain || !fs::is_regular_file(unpackedDir / mainPath)) {
                payloadRoot = fs::u8path(updater::discoverExtractedPayload(unpackedDir.u8string()));
            }
            updater::log("Payload root: " + payloadRoot.u8string());

            if (!fs::is_directory(payloadRoot) || fs::is_empty(payloadRoot)) {
                throw std::runtime_error("Update payload is empty");
            }
            if (args.startMain && !fs::is_regular_file(payloadRoot / mainPath)) {
                throw std::runtime_error("Launch executable missing in payload: " + args.mainExe);
            }

            const bool replaceBundle = appDir.extension() == ".app" && payloadRoot.extension() == ".app";
            if (replaceBundle && !fs::is_regular_file(payloadRoot / "Contents/Info.plist"))
                throw std::runtime_error("Application bundle is missing Contents/Info.plist");
            installationStarted = true;
            if (replaceBundle) {
                // Keep updater working state until commit/rollback, but remove
                // obsolete bundle content instead of leaving stale resources.
                for (const auto& item : fs::directory_iterator(appDir)) {
                    const auto name = item.path().filename().u8string();
                    if (name != args.updaterDir && name != args.backupDirName && name != ".update-lock")
                        fs::remove_all(item.path());
                }
            }
            updater::copyTreeOverlayWithKeep(payloadRoot.u8string(), appDir.u8string(), keepRelUpdate);

            if (args.startMain) {
                const auto executable = appDir / mainPath;
                if (!updater::startMainExe(executable.u8string(), appDir.u8string(), args.mainArgs)) {
                    throw std::runtime_error("Failed to launch: " + executable.u8string());
                }
            }

            updater::log("Updater finished.");
            exitCode = 0;
        } catch (const std::exception& e) {
            // 更新失败：根据配置决定是否从备份回滚
            updater::log(std::string("Update failed: ") + e.what());
            if (!installationStarted) {
                exitCode = 1;
            } else if (args.rollbackOnFailure) {
                try {
                    updater::rollbackFromBackup(backupDir.u8string(), appDir.u8string(), keepRelRollback);
                    exitCode = 20;
                } catch (const std::exception& rbE) {
                    updater::log(std::string("Rollback failed: ") + rbE.what());
                    exitCode = 21;
                }
            } else {
                exitCode = 22;
            }

            if (exitCode == 21 || exitCode == 22) {
                updater::log("Recovery backup retained: " + backupDir.u8string());
                backupRootPathToCleanup.clear();
            }
        }
    } catch (const std::exception& e) {
        updater::log(std::string("Fatal error: ") + e.what());
        exitCode = 1;
    }

exit_main:
    if (!backupRootPathToCleanup.empty()) {
        updater::safeRemoveTree(backupRootPathToCleanup, "backup");
    }
    if (!updaterDirPathToCleanup.empty()) {
        updater::safeRemoveTree(updaterDirPathToCleanup, "work directory");
    }

    if (!lockPathToCleanup.empty()) {
        updater::safeRemoveTree(lockPathToCleanup, "installation lock");
    }
#ifdef __APPLE__
    terminal.setResult(exitCode);
#endif
#ifdef _WIN32
    waitForConsoleClose(exitCode);
#endif
    return exitCode;
}
