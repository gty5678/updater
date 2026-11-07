#include "fs_ops.hpp"
#include "utils.hpp"
#include <filesystem>
#include <functional>
#include <algorithm>
#include <chrono>
#include <thread>
#ifdef __APPLE__
#include <strings.h>
#endif

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

static bool isKeptName(const std::string& name, const std::set<std::string>& keep) {
#ifdef _WIN32
    const auto wideName = std::filesystem::u8path(name).wstring();
    return std::any_of(keep.begin(), keep.end(), [&](const auto& candidate) {
        const auto wideCandidate = std::filesystem::u8path(candidate).wstring();
        return CompareStringOrdinal(wideName.c_str(), -1, wideCandidate.c_str(), -1, TRUE) == CSTR_EQUAL;
    });
#elif defined(__APPLE__)
    return std::any_of(keep.begin(), keep.end(), [&](const auto& candidate) {
        return strcasecmp(name.c_str(), candidate.c_str()) == 0;
    });
#else
    return keep.count(name) != 0;
#endif
}

void copyTreeOverlayWithKeep(const std::string& srcRoot,
                             const std::string& destRoot,
                             const std::set<std::string>& keepRel) {
    std::filesystem::path dest = std::filesystem::u8path(destRoot);
    std::filesystem::path src = std::filesystem::u8path(srcRoot);

    // Overlay only entries provided by the package. Never follow destination
    // symlinks while writing, and retain files absent from the new payload.
    std::function<void(const std::filesystem::path&, const std::filesystem::path&)> overlay;
    overlay = [&](const auto& source, const auto& target) {
        const auto status = std::filesystem::symlink_status(source);
        const auto targetStatus = std::filesystem::symlink_status(target);
        if (std::filesystem::is_directory(status)) {
            if (!std::filesystem::is_directory(targetStatus)) {
                std::filesystem::remove_all(target);
                std::filesystem::create_directories(target);
            }
            for (const auto& child : std::filesystem::directory_iterator(source))
                overlay(child.path(), target / child.path().filename());
            std::filesystem::permissions(target, status.permissions());
        } else {
            std::filesystem::remove_all(target);
            std::filesystem::copy(source, target,
                std::filesystem::copy_options::copy_symlinks |
                std::filesystem::copy_options::overwrite_existing);
        }
    };
    for (const auto& item : std::filesystem::directory_iterator(src)) {
        if (isKeptName(item.path().filename().u8string(), keepRel)) continue;
        const auto target = dest / item.path().filename();
        runFsWithRetry("Overlay: " + target.u8string(), [&] { overlay(item.path(), target); });
    }
}

void backupAppDir(const std::string& appDir,
                  const std::string& backupDir,
                  const std::string& updaterDirName,
                  const std::string& backupRootName) {
    std::filesystem::path app = std::filesystem::u8path(appDir);
    std::filesystem::path backup = std::filesystem::u8path(backupDir);
    std::filesystem::create_directories(backup);

    const std::set<std::string> excluded{updaterDirName, backupRootName, ".update-lock"};
    for (const auto& item : std::filesystem::directory_iterator(app)) {
        std::filesystem::path namePath = item.path().filename();
        std::string nameUtf8 = namePath.u8string();
        if (isKeptName(nameUtf8, excluded)) continue;

        std::filesystem::path dst = backup / namePath;
        runFsWithRetry("Copy during backup: " + item.path().u8string() + " -> " + dst.u8string(), [&item, &dst]() {
            std::filesystem::copy(item.path(), dst,
                std::filesystem::copy_options::recursive |
                std::filesystem::copy_options::copy_symlinks |
                std::filesystem::copy_options::overwrite_existing);
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
    // Rollback restores the exact snapshot, including removal of new files.
    for (const auto& item : std::filesystem::directory_iterator(std::filesystem::u8path(appDir))) {
        if (!isKeptName(item.path().filename().u8string(), keepRel))
            std::filesystem::remove_all(item.path());
    }
    copyTreeOverlayWithKeep(backupDir, appDir, keepRel);
    log("Rollback: completed.");
}

void safeRemoveTree(const std::string& path, const std::string& label) {
    std::filesystem::path p = std::filesystem::u8path(path);
    try {
        if (!std::filesystem::exists(p)) return;
        std::filesystem::remove_all(p);
        log("Cleanup removed " + label + ": " + path);
    } catch (const std::exception& e) {
        log("Cleanup skipped " + label + " (remove failed): " + e.what());
    }
}

}  // namespace updater
