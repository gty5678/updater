/**
 * DarkEye 更新器主程序
 * 负责检查更新、下载安装包、解压覆盖、备份与回滚
 */
#include "download.hpp"
#include "fs_ops.hpp"
#include "json_parse.hpp"
#include "process.hpp"
#include "tarzst_extract.hpp"
#include "utils.hpp"
#include "zip_extract.hpp"
#include <algorithm>
#include <limits>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#endif
#include <chrono>
#include <iostream>
#include <cstring>
#include <set>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

/// 默认的 latest.json 远程地址，用于获取最新版本信息
const char* DEFAULT_LATEST_JSON_URL =
    "https://raw.githubusercontent.com/de4321/darkeye/main/update/latest.json";

/// 命令行参数与配置
struct Args {
    std::string latestJsonUrl = DEFAULT_LATEST_JSON_URL;  ///< latest.json 的 URL
    std::string installDir = ".";                         ///< 应用安装目录
    std::string mainExe = "DarkEye.exe";                  ///< 主程序可执行文件名
    std::string currentVersion;                           ///< 当前本地版本号
    std::vector<std::string> keep = {"data"};             ///< 更新时保留的目录/文件列表
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

/// 检查是否以副本方式运行（--running-from-copy <路径>），若是则返回 true 并填充 outPath
bool hasRunningFromCopy(int argc, char* argv[], std::string& outPath) {
    outPath.clear();
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--running-from-copy" && i + 1 < argc) {
            outPath = argv[i + 1];
            return true;
        }
    }
    return false;
}

/// 解析命令行参数，填充 args
void parseArgs(int argc, char* argv[], Args& args) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--running-from-copy") {
            if (i + 1 < argc) ++i;
            continue;
        }
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
            args.pid = static_cast<uint32_t>(std::stoul(argv[++i]));
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
        } else if (arg == "--local-package" && i + 1 < argc) {
            args.localPackage = argv[++i];
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

/// 启动时欢迎横幅（仅在实际执行更新的进程中打印，复制自身后立即退出的实例不打印）
void printWelcomeBanner() {
    std::cout
        << "\n"
        << "  ============================================================\n"
        << "       DarkEye 版本更新器\n"
        << "       欢迎使用 — 即将检查更新并安装\n"
        << "  ============================================================\n"
        << "\n";
}

}  // namespace

#ifdef _WIN32
/// Windows 下 argv 为系统 ANSI，与内部 UTF-8 路径不一致；从 Unicode 命令行解析为 UTF-8
static std::string wideToUtf8MainArgv(const wchar_t* wide) {
    if (!wide || !*wide) return "";
    int size = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0) return "";
    std::string result(static_cast<size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, result.data(), size, nullptr, nullptr);
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
    //        10=缺少 --current-version, 11=本地安装包路径无效,
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

    // 判断是否已从副本启动（避免覆盖正在运行的 exe）
    std::string runningFromCopyPath;
    bool isCopy = hasRunningFromCopy(argc, argv, runningFromCopyPath);

    // 最外层失败兜底清理（例如下载阶段超时抛异常时）
    std::string updaterDirPathToCleanup;
    std::string backupRootPathToCleanup;

    try {
        Args args;
        parseArgs(argc, argv, args);

        std::filesystem::path appDir =
            std::filesystem::absolute(std::filesystem::u8path(args.installDir));
        std::filesystem::path updaterDir = appDir / std::filesystem::u8path(args.updaterDir);
        std::filesystem::path backupRoot = appDir / std::filesystem::u8path(args.backupDirName);
        updaterDirPathToCleanup = updaterDir.u8string();
        backupRootPathToCleanup = backupRoot.u8string();

        // 若非副本运行，先复制自身到临时目录再以副本方式重新启动
        if (!isCopy) {
            std::string ts = getTimestamp();
            std::filesystem::path copyPath = std::filesystem::temp_directory_path() / ("DarkEyeUpdater_" + ts + ".exe");
            if (updater::copySelfAndLaunch(copyPath.u8string(), appDir.u8string(), argc, argv)) {
                return 0;
            }
        }

        printWelcomeBanner();

        auto keepUser = parseKeep(args.keep);

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
            std::filesystem::create_directories(updaterDir);
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
            auto latest = updater::loadLatestInfo(jsonStr);
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

            std::filesystem::create_directories(updaterDir);

            // 根据 package_url 扩展名决定格式：.tar.zst 或 .zip
            packageExt = packageExtFromUrlOrPath(latest.packageUrl);
            packagePath = updaterDir / ("update" + packageExt);

            // 下载更新包，带进度条
            auto packed = updater::downloadBytes(
                latest.packageUrl,
                args.packageTimeoutSeconds,
                args.downloadRetries,
                [](size_t downloaded, size_t total) {
                    double pct = (total > 0) ? (100.0 * downloaded / total) : 0.0;
                    updater::printProgressBar("Download", pct, downloaded, total, false);
                });
            updater::progressBarEnd();
            {
                std::ofstream of(packagePath, std::ios::binary);
                of.write(reinterpret_cast<const char*>(packed.data()), packed.size());
            }
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
                    exitCode = 3;
                    goto exit_main;
                }
                updater::log("SHA256 verified.");
            } else {
                updater::log("SHA256 not provided or placeholder; skipping verification.");
            }
        }

        // 等待主进程退出，以便安全覆盖文件
        if (args.pid > 0) {
            updater::log("Waiting for main PID " + std::to_string(args.pid) + " to exit...");
            bool ok = updater::waitForPidExit(args.pid, args.waitTimeoutSeconds);
            if (!ok) {
                updater::log("Main process still running after timeout. Continue anyway.");
            }
        } else {
            updater::log("No PID provided; waiting 3 seconds for manual shutdown...");
            std::this_thread::sleep_for(std::chrono::seconds(3));
        }

        // 创建带时间戳的备份目录并备份当前应用
        std::string ts = getTimestamp();
        std::filesystem::path backupDir = backupRoot / ("backup_" + ts);
        updater::log("Backing up app to: " + backupDir.u8string());

        updater::ensureEmptyOrCreate(backupDir.u8string());
        updater::backupAppDir(appDir.u8string(), backupDir.u8string(), args.updaterDir, args.backupDirName);

        // 更新时保留的目录（含 .updater 和 .updater_backup）
        auto keepRelUpdate = keepUser;
        keepRelUpdate.insert(args.updaterDir);
        keepRelUpdate.insert(args.backupDirName);
        auto keepRelRollback = keepRelUpdate;

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
            std::string payloadRoot = updater::discoverExtractedPayload(unpackedDir.u8string());
            updater::log("Payload root: " + payloadRoot);

            // 将 payload 覆盖到应用目录，保留 keep 中的目录
            updater::log("Overlay updating app/ (keep: ...)...");
            updater::copyTreeOverlayWithKeep(payloadRoot, appDir.u8string(), keepRelUpdate);

#ifdef _WIN32
            // 更新完成后启动主程序
            if (args.startMain) {
                std::filesystem::path mainExePath = appDir / std::filesystem::u8path(args.mainExe);
                if (!std::filesystem::exists(mainExePath)) {
                    throw std::runtime_error("Cannot find main exe after update: " + mainExePath.u8string());
                }
                std::string mainArgs = updater::trim(args.mainArgs);
                updater::log("Starting main: " + mainExePath.u8string() + (mainArgs.empty() ? "" : " ") + mainArgs);
                if (!updater::startMainExe(mainExePath.u8string(), appDir.u8string(), mainArgs)) {
                    updater::log("Failed to start main executable.");
                }
            } else {
                updater::log("Skip starting main executable (--no-start-main).");
            }
#else
            updater::log("Starting main not implemented on this platform.");
#endif

            // 清理临时目录：.updater 和 .updater_backup
            updater::safeRemoveTree(updaterDir.u8string(), ".updater");
            updater::safeRemoveTree(backupRoot.u8string(), ".updater_backup");

            updater::log("Updater finished.");
            exitCode = 0;
        } catch (const std::exception& e) {
            // 更新失败：根据配置决定是否从备份回滚
            updater::log(std::string("Update failed: ") + e.what());
            if (args.rollbackOnFailure) {
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

            // 失败回退完成后：从用户视角不应残留更新器临时目录
            updater::safeRemoveTree(updaterDir.u8string(), ".updater");
            updater::safeRemoveTree(backupRoot.u8string(), ".updater_backup");
        }

#ifdef _WIN32
        // 若从副本运行，更新完成后安排删除临时副本 exe
        if (isCopy && !runningFromCopyPath.empty()) {
            updater::scheduleSelfDelete(runningFromCopyPath);
        }
#endif
    } catch (const std::exception& e) {
        // 顶层异常：参数解析、复制自身等早期错误
        updater::log(std::string("Fatal error: ") + e.what());
        exitCode = 1;

        // 下载阶段失败也要从用户视角保证不残留更新器目录
        if (!updaterDirPathToCleanup.empty()) {
            updater::safeRemoveTree(updaterDirPathToCleanup, ".updater");
        }
        if (!backupRootPathToCleanup.empty()) {
            updater::safeRemoveTree(backupRootPathToCleanup, ".updater_backup");
        }
    }

exit_main:
#ifdef _WIN32
    // 成功或失败均暂停，便于查看完整日志
    std::cout << "\n按回车键退出...";
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    std::cin.get();
#endif

    return exitCode;
}
