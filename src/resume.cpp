#include "resume.hpp"
#include "utils.hpp"
#include <filesystem>
#include <nlohmann/json.hpp>
#include <picosha2.h>
#include <cstdio>
#include <stdexcept>

namespace updater {
namespace fs = std::filesystem;
std::string downloadCachePath(const std::string& installDir, const std::string& url,
                              const std::string& sha) {
    const auto identity = nlohmann::json::array({installDir, url, sha}).dump();
    const auto key = picosha2::hash256_hex_string(identity);
    const auto root = fs::temp_directory_path() / "updater-downloads";
    if (fs::is_symlink(fs::symlink_status(root)))
        throw std::runtime_error("Download cache must not be a symlink");
    fs::create_directories(root);
    return (root / (key + ".part")).u8string();
}

ResumableFile::ResumableFile(const std::string& url, const std::string& path,
                           const std::string& sha, DownloadProgressFn progress)
    : path_(path), url_(url), sha_(sha), progress_(std::move(progress)) {
    const auto file = fs::u8path(path_);
    const auto meta = fs::u8path(path_ + ".json");
    for (const auto& p : {file, meta}) {
        const auto status = fs::symlink_status(p);
        if (fs::exists(status) && !fs::is_regular_file(status))
            throw std::runtime_error("Download cache must be a regular file");
    }
    try {
        std::ifstream input(meta);
        auto j = nlohmann::json::parse(input);
        if (j.at("url") == url_ && j.at("sha256") == sha_) {
            validator_ = j.value("validator", "");
            total_ = j.value("total", size_t{0});
            if (fs::exists(file)) offset_ = fs::file_size(file);
        }
    } catch (const std::exception&) { offset_ = 0; validator_.clear(); }
    // Without a validator or a final digest, mixing responses cannot be verified.
    if (validator_.empty() && isPlaceholderSha256(sha_)) offset_ = 0;
    if (total_ && offset_ > total_) offset_ = 0;
    if (offset_) log("Resuming package download from byte " + std::to_string(offset_));
}

void ResumableFile::save() {
    std::ofstream out(fs::u8path(path_ + ".json"), std::ios::trunc);
    out << nlohmann::json{{"url", url_}, {"sha256", sha_},
                          {"validator", validator_}, {"total", total_}}.dump();
    out.close();
    if (!out) throw std::runtime_error("Cannot save download metadata");
}

void ResumableFile::begin(long status, const std::map<std::string, std::string>& headers) {
    auto header = [&](const char* name) {
        const auto it = headers.find(name);
        return it == headers.end() ? std::string{} : trim(it->second);
    };
    const auto range = header("content-range");
    const auto encoding = header("content-encoding");
    if (!encoding.empty() && encoding != "identity")
        throw std::runtime_error("Encoded package response cannot be resumed safely");
    if (status == 416) {
        unsigned long long size = 0;
        if (std::sscanf(range.c_str(), "bytes */%llu", &size) == 1 &&
            offset_ && size == offset_ && total_ == offset_ &&
            (!validator_.empty() || !isPlaceholderSha256(sha_))) {
            accepted_ = true;
            return;
        }
        fs::remove(fs::u8path(path_));
        fs::remove(fs::u8path(path_ + ".json"));
        throw std::runtime_error("HTTP 416: discarded invalid download offset");
    }
    if (status != 200 && status != 206)
        throw std::runtime_error("HTTP " + std::to_string(status));
    auto validator = header("etag");
    if (validator.rfind("W/", 0) == 0) validator.clear();
    if (validator.empty()) validator = header("last-modified");
    size_t start = 0;
    if (status == 206) {
        unsigned long long first = 0, last = 0, size = 0;
        if (std::sscanf(range.c_str(), "bytes %llu-%llu/%llu", &first, &last, &size) != 3 ||
            first != offset_ || last < first || last >= size ||
            (total_ && size != total_) ||
            (!validator_.empty() && !validator.empty() && validator != validator_)) {
            fs::remove(fs::u8path(path_));
            fs::remove(fs::u8path(path_ + ".json"));
            throw std::runtime_error("Invalid Content-Range or changed package validator");
        }
        start = offset_;
        total_ = static_cast<size_t>(size);
    } else {
        // Servers can ignore Range, or If-Range can select a new object. Never append a 200.
        const auto length = header("content-length");
        total_ = length.empty() ? 0 : std::stoull(length);
    }
    offset_ = start;
    if (!validator.empty() || status == 200) validator_ = validator;
    stream_.open(fs::u8path(path_), std::ios::binary |
                 (start ? std::ios::app : std::ios::trunc));
    if (!stream_) throw std::runtime_error("Cannot open package download file");
    save();
    accepted_ = true;
}

void ResumableFile::write(const void* data, size_t length) {
    if (!accepted_ || !stream_.is_open()) throw std::runtime_error("Unexpected HTTP body");
    if (total_ && length > total_ - offset_) throw std::runtime_error("Package exceeds Content-Range");
    stream_.write(static_cast<const char*>(data), length);
    stream_.flush();
    if (!stream_) throw std::runtime_error("Cannot write package download file");
    offset_ += length;
    if (progress_) progress_(offset_, total_);
}
void ResumableFile::finish() {
    if (!accepted_ || (total_ && offset_ != total_))
        throw std::runtime_error("Incomplete package response");
    if (stream_.is_open()) { stream_.close(); if (!stream_) throw std::runtime_error("Cannot close package file"); }
    if (progress_) progress_(offset_, total_);
}
}
