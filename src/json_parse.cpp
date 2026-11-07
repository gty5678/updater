#include "json_parse.hpp"
#include "utils.hpp"
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <algorithm>
#include <cctype>

namespace updater {

LatestInfo loadLatestInfo(const std::string& jsonContent, const std::string& platform) {
    auto j = nlohmann::json::parse(jsonContent);

    LatestInfo info;
    info.latestVersion = trim(j.value("latestVersion", ""));
    info.releaseNotes = trim(j.value("releaseNotes", ""));

    const nlohmann::json* package = nullptr;
    if (j.contains("packages")) {
        const auto& packages = j.at("packages");
        if (!packages.is_object() || !packages.contains(platform)) {
            throw std::runtime_error("Manifest has no package for " + platform);
        }
        package = &packages.at(platform);
    } else if (j.contains("package")) {
        package = &j.at("package");
    }
    if (package && package->is_object()) {
        info.packageUrl = trim(package->value("url", ""));
        info.packageSha256 = trim(package->value("sha256", ""));
    }

    if (info.latestVersion.empty() || info.packageUrl.empty()) {
        throw std::runtime_error("latest.json missing fields: latestVersion/package.url");
    }

    if (j.contains("packages") &&
        (info.packageSha256.size() != 64 ||
         !std::all_of(info.packageSha256.begin(), info.packageSha256.end(),
                      [](unsigned char c) { return std::isxdigit(c) != 0; }) ||
         info.packageSha256.find_first_not_of('0') == std::string::npos)) {
        throw std::runtime_error("Platform package requires a valid SHA256");
    }

    return info;
}

}  // namespace updater
