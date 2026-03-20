#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace updater {

using DownloadProgressFn = std::function<void(size_t downloaded, size_t total)>;

std::vector<uint8_t> downloadBytes(const std::string& url,
                                  int timeoutSeconds = 20,
                                  int retries = 3,
                                  DownloadProgressFn onProgress = nullptr);

}  // namespace updater
