#pragma once

#ifdef _WIN32
#include <cstdint>
#include <string>
#include <vector>

namespace updater {

bool waitForPidExit(uint32_t pid, int timeoutSeconds);

bool startMainExe(const std::string& exePath,
                  const std::string& workingDir,
                  const std::string& mainArgs);

/** Get current process executable path (UTF-8) */
std::string getCurrentExePath();

/**
 * Copy self to destPath, launch the copy with all args + --running-from-copy <destPath>, then exit.
 * Returns true if copy was launched (caller should exit); false on failure.
 */
bool copySelfAndLaunch(const std::string& destPath,
                       const std::string& workingDir,
                       int argc, char* argv[]);

/** Schedule deletion of exePath after this process exits (via background helper). */
void scheduleSelfDelete(const std::string& exePath);

}  // namespace updater
#endif
