#include "download.hpp"
#include "fs_ops.hpp"
#include "json_parse.hpp"
#include "process.hpp"
#include "utils.hpp"
#include "zip_extract.hpp"
#include <algorithm>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif
#include <chrono>
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

const char* DEFAULT_LATEST_JSON_URL =
    "https://raw.githubusercontent.com/de4321/darkeye/main/update/latest.json";

struct Args {
    std::string latestJsonUrl = DEFAULT_LATEST_JSON_URL;
    std::string installDir = ".";
    std::string mainExe = "DarkEye.exe";
    std::string currentVersion;
    std::vector<std::string> keep = {"data"};
    bool rollbackOnFailure = true;
    uint32_t pid = 0;
    int waitTimeoutSeconds = 120;
    int downloadTimeoutSeconds = 30;
    int downloadRetries = 3;
    int packageTimeoutSeconds = 120;
    std::string mainArgs;
    bool startMain = true;
    std::string updaterDir = ".updater";
    std::string backupDirName = ".updater_backup";
};

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
        }
    }
}

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

}  // namespace

int main(int argc, char* argv[]) {
    int exitCode = 0;

#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif

    try {
        Args args;
        parseArgs(argc, argv, args);

        std::filesystem::path appDir = std::filesystem::absolute(args.installDir);
        std::filesystem::path updaterDir = appDir / args.updaterDir;
        std::filesystem::path backupRoot = appDir / args.backupDirName;

        auto keepUser = parseKeep(args.keep);

        updater::log("Working in: " + appDir.u8string());

        std::string localVersion = updater::trim(args.currentVersion);
        if (localVersion.empty()) {
            updater::log("Missing --current-version. Example: --current-version 1.1.1");
            return 10;
        }
        updater::log("Local version: " + localVersion);

        auto rawJson = updater::downloadBytes(args.latestJsonUrl,
                                             args.downloadTimeoutSeconds,
                                             args.downloadRetries);
        std::string jsonStr(rawJson.begin(), rawJson.end());
        auto latest = updater::loadLatestInfo(jsonStr);
        updater::log("Remote version: " + latest.latestVersion);

        if (!updater::isNewer(latest.latestVersion, localVersion)) {
            updater::log("No update needed.");
            return 0;
        }

        updater::log("Update is available. Preparing package download...");

        if (latest.packageUrl.empty()) {
            updater::log("package_url is empty.");
            return 2;
        }

        std::filesystem::create_directories(updaterDir);
        std::filesystem::path zipPath = updaterDir / "update.zip";

        auto packed = updater::downloadBytes(latest.packageUrl,
                                             args.packageTimeoutSeconds,
                                             args.downloadRetries);
        {
            std::ofstream of(zipPath, std::ios::binary);
            of.write(reinterpret_cast<const char*>(packed.data()), packed.size());
        }
        updater::log("Downloaded update package: " + zipPath.u8string());

        if (!updater::isPlaceholderSha256(latest.packageSha256)) {
            std::string got = updater::sha256File(zipPath.u8string());
            std::string exp = latest.packageSha256;
            for (auto& c : got) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            for (auto& c : exp) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (got != exp) {
                updater::log("SHA256 mismatch. expected=" + exp + " got=" + got);
                return 3;
            }
            updater::log("SHA256 verified.");
        } else {
            updater::log("SHA256 not provided or placeholder; skipping verification.");
        }

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

        std::string ts = getTimestamp();
        std::filesystem::path backupDir = backupRoot / ("backup_" + ts);
        updater::log("Backing up app to: " + backupDir.u8string());

        updater::ensureEmptyOrCreate(backupDir.u8string());
        updater::backupAppDir(appDir.u8string(), backupDir.u8string(), args.updaterDir, args.backupDirName);

        auto keepRelUpdate = keepUser;
        keepRelUpdate.insert(args.updaterDir);
        keepRelUpdate.insert(args.backupDirName);
        auto keepRelRollback = keepRelUpdate;

        try {
            updater::log("Unpacking package...");
            std::filesystem::path unpackedDir = updaterDir / "unpacked";
            updater::ensureEmptyOrCreate(unpackedDir.u8string());

            updater::extractZipToDir(zipPath.u8string(), unpackedDir.u8string());

            std::string payloadRoot = updater::discoverExtractedPayload(unpackedDir.u8string());
            updater::log("Payload root: " + payloadRoot);

            updater::log("Overlay updating app/ (keep: ...)...");
            updater::copyTreeOverlayWithKeep(payloadRoot, appDir.u8string(), keepRelUpdate);

#ifdef _WIN32
            if (args.startMain) {
                std::filesystem::path mainExePath = appDir / args.mainExe;
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

            updater::safeRemoveTree(updaterDir.u8string(), ".updater");
            updater::safeRemoveTree(backupRoot.u8string(), ".updater_backup");

            updater::log("Updater finished.");
            exitCode = 0;
        } catch (const std::exception& e) {
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
        }
    } catch (const std::exception& e) {
        updater::log(std::string("Fatal error: ") + e.what());
        exitCode = 1;
    }

    return exitCode;
}
