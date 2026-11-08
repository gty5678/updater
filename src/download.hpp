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

// Package transfers stream to disk and preserve validated partial data on failure.
void downloadFile(const std::string& url, const std::string& path,
                  const std::string& sha256, int timeoutSeconds, int retries,
                  DownloadProgressFn onProgress = nullptr);
std::string downloadCachePath(const std::string& installDir, const std::string& url,
                              const std::string& sha256);

}  // namespace updater
