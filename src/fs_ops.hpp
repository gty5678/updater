#pragma once

#include <set>
#include <string>

namespace updater {

void ensureEmptyOrCreate(const std::string& path);

void copyTreeOverlayWithKeep(const std::string& srcRoot,
                             const std::string& destRoot,
                             const std::set<std::string>& keepRel);

void backupAppDir(const std::string& appDir,
                  const std::string& backupDir,
                  const std::string& updaterDirName,
                  const std::string& backupRootName);

void rollbackFromBackup(const std::string& backupDir,
                       const std::string& appDir,
                       const std::set<std::string>& keepRel);

void safeRemoveTree(const std::string& path, const std::string& label);

}  // namespace updater
