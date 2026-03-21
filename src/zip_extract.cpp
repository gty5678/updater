#include "zip_extract.hpp"
#include "utils.hpp"
#include "miniz.h"
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace updater {

namespace {

size_t zipWriteToOfstream(void* pOpaque, mz_uint64 ofs, const void* pBuf, size_t n) {
    (void)ofs;
    auto* os = static_cast<std::ofstream*>(pOpaque);
    os->write(static_cast<const char*>(pBuf), static_cast<std::streamsize>(n));
    return os->good() ? n : 0;
}

}  // namespace

void extractZipToDir(const std::string& zipPath, const std::string& destDir,
                     ExtractProgressFn onProgress) {
    // miniz 的 fopen 使用窄路径（非 UTF-8）；整包读入内存后用 init_mem，解压时用 path 打开目标文件
    std::ifstream ifs(std::filesystem::u8path(zipPath), std::ios::binary | std::ios::ate);
    if (!ifs) {
        throw std::runtime_error("Failed to open zip: " + zipPath);
    }
    std::streamsize zipSize = ifs.tellg();
    if (zipSize < 0) {
        throw std::runtime_error("Failed to size zip: " + zipPath);
    }
    ifs.seekg(0);
    std::vector<uint8_t> zipData(static_cast<size_t>(zipSize));
    if (zipSize > 0 && !ifs.read(reinterpret_cast<char*>(zipData.data()), zipSize)) {
        throw std::runtime_error("Failed to read zip: " + zipPath);
    }

    mz_zip_archive zip{};
    if (!mz_zip_reader_init_mem(&zip, zipData.data(), zipData.size(), 0)) {
        throw std::runtime_error("Failed to open zip: " + zipPath);
    }

    mz_uint numFiles = mz_zip_reader_get_num_files(&zip);
    std::filesystem::path destBase = std::filesystem::u8path(destDir);

    for (mz_uint i = 0; i < numFiles; ++i) {
        if (onProgress) {
            onProgress(static_cast<size_t>(i), static_cast<size_t>(numFiles));
        }
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
            std::ofstream out(destPathNorm, std::ios::binary);
            if (!out) {
                mz_zip_reader_end(&zip);
                throw std::runtime_error("Failed to extract: " + filename);
            }
            if (!mz_zip_reader_extract_to_callback(&zip, i, zipWriteToOfstream, &out, 0)) {
                mz_zip_reader_end(&zip);
                throw std::runtime_error("Failed to extract: " + filename);
            }
        }
    }

    if (onProgress) {
        onProgress(static_cast<size_t>(numFiles), static_cast<size_t>(numFiles));
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
        return singleDir.u8string();
    }
    return unpackedDir;
}

}  // namespace updater
