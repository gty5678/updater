#include "json_parse.hpp"
#include "utils.hpp"
#include "json.hpp"
#include <stdexcept>

namespace updater {

LatestInfo loadLatestInfo(const std::string& jsonContent) {
    auto j = nlohmann::json::parse(jsonContent);

    LatestInfo info;
    info.latestVersion = trim(j.value("latestVersion", ""));
    info.releaseNotes = trim(j.value("releaseNotes", ""));

    if (j.contains("package") && j["package"].is_object()) {
        auto& pkg = j["package"];
        info.packageUrl = trim(pkg.value("url", ""));
        info.packageSha256 = trim(pkg.value("sha256", ""));
    }

    if (info.latestVersion.empty() || info.packageUrl.empty()) {
        throw std::runtime_error("latest.json missing fields: latestVersion/package.url");
    }

    return info;
}

}  // namespace updater
