#pragma once

#ifdef _WIN32
#include <cstdint>
#include <string>

namespace updater {

bool waitForPidExit(uint32_t pid, int timeoutSeconds);

bool startMainExe(const std::string& exePath,
                  const std::string& workingDir,
                  const std::string& mainArgs);

}  // namespace updater
#endif
