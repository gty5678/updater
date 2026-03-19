#include "utils.hpp"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include "picosha2.h"

namespace updater {

void log(const std::string& msg) {
    std::cout << "[updater] " << msg << std::endl;
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
