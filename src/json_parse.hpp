#pragma once

#include <string>

namespace updater {

struct LatestInfo {
    std::string latestVersion;
    std::string releaseNotes;
    std::string packageUrl;
    std::string packageSha256;
};

LatestInfo loadLatestInfo(const std::string& jsonContent);

}  // namespace updater
