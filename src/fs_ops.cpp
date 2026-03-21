#include "fs_ops.hpp"
#include "utils.hpp"
#include <filesystem>
#include <functional>
#include <algorithm>
#include <chrono>
#include <thread>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace updater {

static bool isWindowsSharingViolation(int err) {
#ifdef _WIN32
    return err == 32;
#else
    (void)err;
    return false;
#endif
}

static void runFsWithRetry(const std::string& actionDesc,
                           std::function<void()> func,
                           int retries = 8,
                           double baseSleep = 0.25) {
    for (int i = 0; i < retries; ++i) {
        try {
            func();
            return;
        } catch (const std::exception& e) {
            bool retryable = false;
#ifdef _WIN32
            if (auto* pe = dynamic_cast<const std::system_error*>(&e)) {
                if (pe->code().category() == std::system_category()) {
                    retryable = isWindowsSharingViolation(pe->code().value());
                }
            }
#endif
            if (!retryable || i >= retries - 1) {
                throw;
            }
            double sleepS = std::min(2.0, baseSleep * (i + 1));
            log(actionDesc + " failed (attempt " + std::to_string(i + 1) + "/" + std::to_string(retries) + "): " +
                e.what() + "; retry in " + std::to_string(static_cast<int>(sleepS * 100) / 100.0) + "s...");
            std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(sleepS * 1000)));
        }
    }
}

void ensureEmptyOrCreate(const std::string& path) {
    std::filesystem::path p = std::filesystem::u8path(path);
    if (std::filesystem::exists(p)) {
        std::filesystem::remove_all(p);
    }
    std::filesystem::create_directories(p);
}

static bool shouldKeepInOverlay(const std::string& rel, const std::set<std::string>& keepRel) {
    if (keepRel.count(rel)) return true;
    if (rel.find(".updater/") == 0 || rel.find(".updater_backup/") == 0 ||
        rel == ".updater_backup") return true;
    return false;
}

void copyTreeOverlayWithKeep(const std::string& srcRoot,
                             const std::string& destRoot,
                             const std::set<std::string>& keepRel) {
    std::filesystem::path dest = std::filesystem::u8path(destRoot);
    std::filesystem::path src = std::filesystem::u8path(srcRoot);

    for (const auto& item : std::filesystem::directory_iterator(dest)) {
        std::string rel = std::filesystem::relative(item.path(), dest).u8string();
        std::replace(rel.begin(), rel.end(), '\\', '/');
        if (shouldKeepInOverlay(rel, keepRel)) continue;

        runFsWithRetry("Remove during overlay: " + item.path().u8string(), [&item]() {
            std::filesystem::remove_all(item.path());
        });
    }

    for (const auto& item : std::filesystem::directory_iterator(src)) {
        std::string rel = std::filesystem::relative(item.path(), src).u8string();
        std::replace(rel.begin(), rel.end(), '\\', '/');
        if (keepRel.count(rel)) continue;

        std::filesystem::path dstPath = dest / item.path().filename();
        runFsWithRetry("Copy during overlay: " + item.path().u8string() + " -> " + dstPath.u8string(), [&item, &dstPath]() {
            if (item.is_directory()) {
                std::filesystem::copy(item.path(), dstPath, std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing);
            } else {
                std::filesystem::create_directories(dstPath.parent_path());
                std::filesystem::copy_file(item.path(), dstPath, std::filesystem::copy_options::overwrite_existing);
            }
        });
    }
}

static bool ignoreForBackup(const std::string& name) {
    return name == ".updater" || name.find(".updater") == 0 ||
           name == ".updater_backup" || name.find(".updater_backup") == 0;
}

void backupAppDir(const std::string& appDir,
                  const std::string& backupDir,
                  const std::string& updaterDirName,
                  const std::string& backupRootName) {
    std::filesystem::path app = std::filesystem::u8path(appDir);
    std::filesystem::path backup = std::filesystem::u8path(backupDir);
    std::filesystem::create_directories(backup);

    for (const auto& item : std::filesystem::directory_iterator(app)) {
        std::filesystem::path namePath = item.path().filename();
        std::string nameUtf8 = namePath.u8string();
        if (ignoreForBackup(nameUtf8)) continue;

        std::filesystem::path dst = backup / namePath;
        runFsWithRetry("Copy during backup: " + item.path().u8string() + " -> " + dst.u8string(), [&item, &dst]() {
            if (item.is_directory()) {
                std::filesystem::copy(item.path(), dst, std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing);
            } else {
                std::filesystem::copy_file(item.path(), dst, std::filesystem::copy_options::overwrite_existing);
            }
        });
    }
}

void rollbackFromBackup(const std::string& backupDir,
                       const std::string& appDir,
                       const std::set<std::string>& keepRel) {
    log("Rollback: restoring from backup: " + backupDir);
    std::filesystem::path backup = std::filesystem::u8path(backupDir);
    if (!std::filesystem::exists(backup)) {
        throw std::runtime_error("Backup dir not found: " + backupDir);
    }
    if (!std::filesystem::is_directory(backup)) {
        throw std::runtime_error("Backup path is not a directory: " + backupDir);
    }
    copyTreeOverlayWithKeep(backupDir, appDir, keepRel);
    log("Rollback: completed.");
}

void safeRemoveTree(const std::string& path, const std::string& label) {
    std::filesystem::path p = std::filesystem::u8path(path);
    if (!std::filesystem::exists(p)) return;
    try {
        std::filesystem::remove_all(p);
        log("Cleanup removed " + label + ": " + path);
    } catch (const std::exception& e) {
        log("Cleanup skipped " + label + " (remove failed): " + e.what());
    }
}

}  // namespace updater
