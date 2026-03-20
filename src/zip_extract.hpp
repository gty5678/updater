#pragma once

#include <cstddef>
#include <functional>
#include <string>

namespace updater {

using ExtractProgressFn = std::function<void(size_t current, size_t total)>;

void extractZipToDir(const std::string& zipPath, const std::string& destDir,
                     ExtractProgressFn onProgress = nullptr);

std::string discoverExtractedPayload(const std::string& unpackedDir);

}  // namespace updater
