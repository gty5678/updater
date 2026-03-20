#pragma once

#include <cstddef>
#include <functional>
#include <string>

namespace updater {

using ExtractProgressFn = std::function<void(size_t current, size_t total)>;

void extractTarZstToDir(const std::string& tarzstPath, const std::string& destDir,
                        ExtractProgressFn onProgress = nullptr);

}  // namespace updater
