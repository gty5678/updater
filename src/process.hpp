#pragma once

#include <cstdint>
#include <string>

namespace updater {

bool waitForPidExit(uint32_t pid, int timeoutSeconds);

bool startMainExe(const std::string& exePath,
                  const std::string& workingDir,
                  const std::string& mainArgs);

/** Get current process executable path (UTF-8) */
std::string getCurrentExePath();

}  // namespace updater
