#pragma once

#include <string>

namespace updater {

void extractZipToDir(const std::string& zipPath, const std::string& destDir);

std::string discoverExtractedPayload(const std::string& unpackedDir);

}  // namespace updater
