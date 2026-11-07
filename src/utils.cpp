#include "utils.hpp"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include "picosha2.h"

namespace updater {

std::filesystem::path safeRelativePath(const std::filesystem::path& path) {
    if (path.empty() || path.has_root_path()) {
        throw std::runtime_error("Expected a relative path: " + path.u8string());
    }
    for (const auto& part : path) {
        const auto name = part.u8string();
        if (part == ".." || name.find_first_of(":\\") != std::string::npos) {
            throw std::runtime_error("Unsafe relative path: " + path.u8string());
        }
#ifdef _WIN32
        if (!name.empty() && part != "." && (name.back() == '.' || name.back() == ' ')) {
            throw std::runtime_error("Unsafe Windows path: " + path.u8string());
        }
#endif
    }
    return path.lexically_normal();
}


void log(const std::string& msg) {
    std::cout << "[updater] " << msg << std::endl;
}

static std::string formatBytes(size_t bytes, int mbDecimals) {
    if (bytes < 1024) return std::to_string(bytes) + " B";
    if (bytes < 1024 * 1024) return std::to_string(bytes / 1024) + " KB";
    double mb = static_cast<double>(bytes) / (1024 * 1024);
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(mbDecimals) << mb << " MB";
    return oss.str();
}

void printProgressBar(const std::string& label, double percent, size_t current, size_t total,
                     bool asFiles) {
    const int barWidth = 40;
    // 只对下载显示更高精度：百分比和 MB 都保留两位小数
    const bool isDownload = (label == "Download");
    const int percentDecimals = isDownload ? 2 : 1;
    const int mbDecimals = isDownload ? 2 : 1;
    std::ostringstream line;
    line << "\r[updater] " << label << " ";

    if (total > 0) {
        int pos = static_cast<int>(barWidth * percent / 100.0);
        pos = std::min(pos, barWidth);
        line << "[";
        for (int i = 0; i < barWidth; ++i) {
            line << (i < pos ? "=" : (i == pos ? ">" : " "));
        }
        line << "] " << std::fixed << std::setprecision(percentDecimals) << percent << "% (";
        if (asFiles) {
            line << current << " / " << total << " files)";
        } else {
            line << formatBytes(current, mbDecimals) << " / " << formatBytes(total, mbDecimals) << ")";
        }
    } else {
        int pos = (static_cast<int>(current / 1024) % barWidth);
        line << "[";
        for (int i = 0; i < barWidth; ++i) {
            line << (i == pos ? ">" : " ");
        }
        line << "] " << formatBytes(current, mbDecimals) << " ...";
    }

    std::cout << line.str() << std::flush;
}

void progressBarEnd() {
    std::cout << std::endl;
}

std::string trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

std::vector<int> parseVersion(const std::string& v) {
    std::vector<int> result;
    std::string s = trim(v);
    if (s.empty()) return result;
    if (s.size() >= 1 && (s[0] == 'v' || s[0] == 'V')) {
        s = s.substr(1);
    }
    std::istringstream iss(s);
    std::string part;
    while (std::getline(iss, part, '.')) {
        std::string p = trim(part);
        if (p.empty()) continue;
        try {
            result.push_back(std::stoi(p));
        } catch (...) {
            return {};
        }
    }
    return result;
}

bool isNewer(const std::string& remote, const std::string& local) {
    auto remoteT = parseVersion(remote);
    auto localT = parseVersion(local);
    if (remoteT.empty() || localT.empty()) {
        return trim(remote) != trim(local);
    }
    size_t n = std::max(remoteT.size(), localT.size());
    for (size_t i = 0; i < n; ++i) {
        int r = (i < remoteT.size()) ? remoteT[i] : 0;
        int l = (i < localT.size()) ? localT[i] : 0;
        if (r > l) return true;
        if (r < l) return false;
    }
    return false;
}

std::string sha256File(const std::string& path) {
    std::filesystem::path p = std::filesystem::u8path(path);
    std::ifstream f(p, std::ios::binary);
    if (!f) return "";
    return picosha2::hash256_hex_string(std::istreambuf_iterator<char>(f),
                                        std::istreambuf_iterator<char>());
}

bool isPlaceholderSha256(const std::string& s) {
    std::string t = trim(s);
    for (auto& c : t) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (t.empty()) return true;
    if (t.find("PUT_SHA256") != std::string::npos) return true;
    if (t == std::string(64, '0')) return true;
    return false;
}

}  // namespace updater
