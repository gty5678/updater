#include "zip_extract.hpp"
#include "utils.hpp"
#include "miniz.h"
#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace updater {

namespace {

// ZIP 通用位标志 bit 11：文件名/注释使用 UTF-8（与 miniz 内部 MZ_ZIP_GENERAL_PURPOSE_BIT_FLAG_UTF8 一致）
constexpr mz_uint16 kZipUtf8FilenameFlag = 1u << 11;

std::filesystem::path zipEntryFilenameToPath(const std::string& raw, mz_uint16 bitFlag) {
    if (raw.empty()) {
        return {};
    }
    if (bitFlag & kZipUtf8FilenameFlag) {
        return std::filesystem::u8path(raw);
    }
#ifdef _WIN32
    // 未设 UTF-8 标志时，国内常见 zip 为系统 ANSI（如 GBK）；先试严格 UTF-8（部分工具漏设标志）
    int wlen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, raw.data(),
                                   static_cast<int>(raw.size()), nullptr, 0);
    if (wlen > 0) {
        std::wstring w(static_cast<size_t>(wlen), L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, raw.data(),
                            static_cast<int>(raw.size()), w.data(), wlen);
        return std::filesystem::path(std::move(w));
    }
    wlen = MultiByteToWideChar(CP_ACP, 0, raw.data(), static_cast<int>(raw.size()), nullptr, 0);
    if (wlen > 0) {
        std::wstring w(static_cast<size_t>(wlen), L'\0');
        MultiByteToWideChar(CP_ACP, 0, raw.data(), static_cast<int>(raw.size()), w.data(), wlen);
        return std::filesystem::path(std::move(w));
    }
#endif
    return std::filesystem::u8path(raw);
}

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

    struct ZipReaderGuard {
        mz_zip_archive& archive;
        ~ZipReaderGuard() { mz_zip_reader_end(&archive); }
    } guard{zip};

    mz_uint numFiles = mz_zip_reader_get_num_files(&zip);
    std::filesystem::path destBase = std::filesystem::u8path(destDir);

    std::vector<std::pair<std::filesystem::path, std::filesystem::path>> links;
    for (mz_uint i = 0; i < numFiles; ++i) {
        if (onProgress) {
            onProgress(static_cast<size_t>(i), static_cast<size_t>(numFiles));
        }
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&zip, i, &stat)) {
            throw std::runtime_error("Failed to get file stat in zip");
        }

        std::string filename = stat.m_filename;
        if (filename.empty()) continue;

        std::replace(filename.begin(), filename.end(), '\\', '/');
        const auto destPathNorm = destBase / safeRelativePath(zipEntryFilenameToPath(filename, stat.m_bit_flag));
        const bool unixAttributes = (stat.m_version_made_by >> 8) == 3;
        const auto mode = stat.m_external_attr >> 16;
        const auto fileType = mode & 0170000;
        if (unixAttributes && fileType != 0 && fileType != 0100000 && fileType != 0040000 && fileType != 0120000) {
            throw std::runtime_error("Unsupported ZIP entry type: " + filename);
        }

        if (unixAttributes && fileType == 0120000) {
            size_t size = 0;
            void* data = mz_zip_reader_extract_to_heap(&zip, i, &size, 0);
            if (!data || size == 0 || size > 4096) {
                mz_free(data);
                throw std::runtime_error("Invalid ZIP symlink");
            }
            std::string text(static_cast<const char*>(data), size);
            mz_free(data);
            const auto target = std::filesystem::u8path(text);
            if (target.is_absolute() || text.find('\0') != std::string::npos)
                throw std::runtime_error("Unsafe ZIP symlink");
            safeRelativePath((destPathNorm.parent_path().lexically_relative(destBase) / target).lexically_normal());
            links.emplace_back(destPathNorm, target);
            continue;
        }

        bool isDir = mz_zip_reader_is_file_a_directory(&zip, i) ||
                     (!filename.empty() && (filename.back() == '/' || filename.back() == '\\'));
        if (isDir) {
            std::filesystem::create_directories(destPathNorm);
        } else {
            std::filesystem::create_directories(destPathNorm.parent_path());
            std::ofstream out(destPathNorm, std::ios::binary);
            if (!out) {
                throw std::runtime_error("Failed to extract: " + filename);
            }
            if (!mz_zip_reader_extract_to_callback(&zip, i, zipWriteToOfstream, &out, 0)) {
                throw std::runtime_error("Failed to extract: " + filename);
            }
            out.close();
            if (!out) throw std::runtime_error("Failed to write: " + filename);
#ifndef _WIN32
            if (unixAttributes && (mode & 0777)) {
                std::filesystem::permissions(destPathNorm,
                    static_cast<std::filesystem::perms>(mode & 0777));
            }
#endif
        }
    }

    // Links are installed last, so archive entries cannot write through them.
    for (const auto& link : links) {
        const auto resolved = std::filesystem::weakly_canonical(link.first.parent_path() / link.second);
        const auto relative = resolved.lexically_relative(std::filesystem::weakly_canonical(destBase));
        safeRelativePath(relative);
        for (auto parent = link.first.parent_path(); parent != destBase && !parent.empty(); parent = parent.parent_path())
            if (std::filesystem::is_symlink(std::filesystem::symlink_status(parent)))
                throw std::runtime_error("ZIP symlink parent is a link");
        std::filesystem::create_directories(link.first.parent_path());
        if (std::filesystem::exists(std::filesystem::symlink_status(link.first)))
            throw std::runtime_error("Duplicate ZIP symlink path");
        std::filesystem::create_symlink(link.second, link.first);
    }

    if (onProgress) {
        onProgress(static_cast<size_t>(numFiles), static_cast<size_t>(numFiles));
    }

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
