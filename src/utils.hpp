#pragma once

#include <string>
#include <vector>

namespace updater {

void log(const std::string& msg);

std::vector<int> parseVersion(const std::string& v);
bool isNewer(const std::string& remote, const std::string& local);

std::string sha256File(const std::string& path);

bool isPlaceholderSha256(const std::string& s);

std::string trim(const std::string& s);

}  // namespace updater
