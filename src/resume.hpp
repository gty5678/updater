#pragma once
#include "download.hpp"
#include <fstream>
#include <map>

namespace updater {
// Shared HTTP validation for both native NSURLSession and libcurl backends.
class ResumableFile {
    std::string path_, url_, sha_, validator_;
    std::ofstream stream_;
    DownloadProgressFn progress_;
    size_t offset_ = 0, total_ = 0;
    bool accepted_ = false;
    void save();
public:
    ResumableFile(const std::string& url, const std::string& path,
                  const std::string& sha, DownloadProgressFn progress);
    size_t offset() const { return offset_; }
    const std::string& validator() const { return validator_; }
    void begin(long status, const std::map<std::string, std::string>& headers);
    void write(const void* data, size_t length);
    void finish();
};
}
