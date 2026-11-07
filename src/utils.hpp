#pragma once

#include <filesystem>

#include <cstddef>
#include <string>
#include <vector>

namespace updater {

std::filesystem::path safeRelativePath(const std::filesystem::path& path);

void log(const std::string& msg);

void printProgressBar(const std::string& label, double percent, size_t current, size_t total,
                     bool asFiles = false);

void progressBarEnd();

std::vector<int> parseVersion(const std::string& v);
bool isNewer(const std::string& remote, const std::string& local);

std::string sha256File(const std::string& path);

bool isPlaceholderSha256(const std::string& s);

std::string trim(const std::string& s);

}  // namespace updater
