#include "zip_extract.hpp"
#include "utils.hpp"
#include "miniz.h"
#include <filesystem>
#include <stdexcept>

namespace updater {

void extractZipToDir(const std::string& zipPath, const std::string& destDir) {
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_file(&zip, zipPath.c_str(), 0)) {
        throw std::runtime_error("Failed to open zip: " + zipPath);
    }

    mz_uint numFiles = mz_zip_reader_get_num_files(&zip);
    std::filesystem::path destBase = std::filesystem::u8path(destDir);

    for (mz_uint i = 0; i < numFiles; ++i) {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&zip, i, &stat)) {
            mz_zip_reader_end(&zip);
            throw std::runtime_error("Failed to get file stat in zip");
        }

        std::string filename = stat.m_filename;
        if (filename.empty()) continue;

        std::filesystem::path filenamePath = std::filesystem::u8path(filename);
        std::filesystem::path destPath = destBase / filenamePath;
        std::filesystem::path destPathNorm = std::filesystem::path(destPath).lexically_normal();

        bool isDir = mz_zip_reader_is_file_a_directory(&zip, i) ||
                     (!filename.empty() && (filename.back() == '/' || filename.back() == '\\'));
        if (isDir) {
            std::filesystem::create_directories(destPathNorm);
        } else {
            std::filesystem::create_directories(destPathNorm.parent_path());
            std::string destPathUtf8 = destPathNorm.u8string();
            if (!mz_zip_reader_extract_to_file(&zip, i, destPathUtf8.c_str(), 0)) {
                mz_zip_reader_end(&zip);
                throw std::runtime_error("Failed to extract: " + filename);
            }
        }
    }

    mz_zip_reader_end(&zip);
}

std::string discoverExtractedPayload(const std::string& unpackedDir) {
    std::filesystem::path p = std::filesystem::u8path(unpackedDir);
    if (!std::filesystem::exists(p) || !std::filesystem::is_directory(p)) {
        return unpackedDir;
    }

    int dirCount = 0;
    std::filesystem::path singleDir;
    int entryCount = 0;

    for (const auto& entry : std::filesystem::directory_iterator(p)) {
        ++entryCount;
        if (entry.is_directory()) {
            ++dirCount;
            singleDir = entry.path();
        }
    }

    if (dirCount == 1 && entryCount == 1) {
        return singleDir.string();
    }
    return unpackedDir;
}

}  // namespace updater
