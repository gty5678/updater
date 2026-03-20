#include "tarzst_extract.hpp"
#include <zstd.h>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace updater {

namespace {

constexpr size_t TAR_BLOCK_SIZE = 512;

// UStar header layout (POSIX ustar format)
struct UstarHeader {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char chksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];   // "ustar\0"
    char version[2]; // "00"
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char padding[12];
};

// Parse octal string to size_t (max 12 digits for size)
size_t parseOctal(const char* p, size_t len) {
    size_t result = 0;
    for (size_t i = 0; i < len && p[i] >= '0' && p[i] <= '7'; ++i) {
        result = result * 8 + static_cast<size_t>(p[i] - '0');
    }
    return result;
}

bool isZeroBlock(const uint8_t* p) {
    for (size_t i = 0; i < TAR_BLOCK_SIZE; ++i) {
        if (p[i] != 0) return false;
    }
    return true;
}

// Build path from prefix + name, normalize slashes
std::string buildPath(const char* prefix, size_t prefixLen, const char* name, size_t nameLen) {
    std::string path;
    if (prefixLen > 0) {
        path.assign(prefix, prefixLen);
        while (!path.empty() && path.back() == '\0') path.pop_back();
        if (!path.empty() && path.back() != '/' && path.back() != '\\') {
            path += '/';
        }
    }
    std::string nameStr(name, nameLen);
    while (!nameStr.empty() && nameStr.back() == '\0') nameStr.pop_back();
    path += nameStr;
    // Normalize: backslash to forward slash
    for (char& c : path) {
        if (c == '\\') c = '/';
    }
    return path;
}

}  // namespace

void extractTarZstToDir(const std::string& tarzstPath, const std::string& destDir,
                        ExtractProgressFn onProgress) {
    // 1. Read compressed file
    std::ifstream ifs(tarzstPath, std::ios::binary | std::ios::ate);
    if (!ifs) {
        throw std::runtime_error("Failed to open: " + tarzstPath);
    }
    std::streamsize fileSize = ifs.tellg();
    ifs.seekg(0);
    std::vector<uint8_t> compressed(static_cast<size_t>(fileSize));
    if (!ifs.read(reinterpret_cast<char*>(compressed.data()), fileSize)) {
        throw std::runtime_error("Failed to read: " + tarzstPath);
    }
    ifs.close();

    size_t compressedSize = compressed.size();
    if (compressedSize == 0) {
        throw std::runtime_error("Empty archive: " + tarzstPath);
    }

    // 2. Decompress with zstd
    unsigned long long decompressedSize = ZSTD_getFrameContentSize(compressed.data(), compressedSize);

    std::vector<uint8_t> tarData;
    if (decompressedSize != ZSTD_CONTENTSIZE_UNKNOWN && decompressedSize != ZSTD_CONTENTSIZE_ERROR) {
        tarData.resize(static_cast<size_t>(decompressedSize));
        size_t result = ZSTD_decompress(tarData.data(), tarData.size(),
                                        compressed.data(), compressedSize);
        if (ZSTD_isError(result)) {
            throw std::runtime_error(std::string("Zstd decompress failed: ") + ZSTD_getErrorName(result));
        }
    } else {
        // Streaming decompression for unknown size
        ZSTD_DCtx* dctx = ZSTD_createDCtx();
        if (!dctx) {
            throw std::runtime_error("ZSTD_createDCtx failed");
        }
        const size_t bufSize = 1024 * 1024;  // 1MB chunks
        std::vector<uint8_t> outBuf(bufSize);
        ZSTD_inBuffer inBuf = {compressed.data(), compressedSize, 0};
        while (inBuf.pos < inBuf.size) {
            ZSTD_outBuffer outBufStruct = {outBuf.data(), bufSize, 0};
            size_t ret = ZSTD_decompressStream(dctx, &outBufStruct, &inBuf);
            if (ZSTD_isError(ret)) {
                ZSTD_freeDCtx(dctx);
                throw std::runtime_error(std::string("Zstd stream decompress failed: ") + ZSTD_getErrorName(ret));
            }
            tarData.insert(tarData.end(), outBuf.data(), outBuf.data() + outBufStruct.pos);
        }
        ZSTD_freeDCtx(dctx);
    }

    const uint8_t* p = tarData.data();
    const uint8_t* end = tarData.data() + tarData.size();
    const size_t totalBytes = tarData.size();

    std::filesystem::path destBase = std::filesystem::u8path(destDir);

    while (p + TAR_BLOCK_SIZE * 2 <= end) {
        if (isZeroBlock(p)) {
            p += TAR_BLOCK_SIZE;
            if (p + TAR_BLOCK_SIZE <= end && isZeroBlock(p)) {
                break;  // End of archive
            }
            continue;
        }

        const auto* h = reinterpret_cast<const UstarHeader*>(p);
        size_t fileSize = parseOctal(h->size, 12);

        // Build path: prefix + name
        size_t nameLen = 0;
        while (nameLen < 100 && h->name[nameLen] != '\0') ++nameLen;
        size_t prefixLen = 0;
        while (prefixLen < 155 && h->prefix[prefixLen] != '\0') ++prefixLen;

        std::string pathStr = buildPath(h->prefix, prefixLen, h->name, nameLen);
        if (pathStr.empty()) {
            p += TAR_BLOCK_SIZE + ((fileSize + TAR_BLOCK_SIZE - 1) / TAR_BLOCK_SIZE) * TAR_BLOCK_SIZE;
            continue;
        }

        std::filesystem::path fullPath = destBase / std::filesystem::u8path(pathStr);
        std::filesystem::path fullPathNorm = fullPath.lexically_normal();

        bool isDir = (h->typeflag == '5') ||
                     (!pathStr.empty() && (pathStr.back() == '/' || pathStr.back() == '\\'));

        if (isDir) {
            std::filesystem::create_directories(fullPathNorm);
        } else if (h->typeflag == '0' || h->typeflag == '\0') {
            std::filesystem::create_directories(fullPathNorm.parent_path());
            const uint8_t* dataStart = p + TAR_BLOCK_SIZE;
            if (dataStart + fileSize <= end) {
                std::ofstream ofs(fullPathNorm, std::ios::binary);
                if (ofs) {
                    ofs.write(reinterpret_cast<const char*>(dataStart), static_cast<std::streamsize>(fileSize));
                }
            }
        }

        size_t blockCount = 1 + (fileSize + TAR_BLOCK_SIZE - 1) / TAR_BLOCK_SIZE;
        size_t bytesProcessed = static_cast<size_t>(p - tarData.data()) + blockCount * TAR_BLOCK_SIZE;
        if (onProgress && totalBytes > 0) {
            onProgress(std::min(bytesProcessed, totalBytes), totalBytes);
        }

        p += TAR_BLOCK_SIZE + ((fileSize + TAR_BLOCK_SIZE - 1) / TAR_BLOCK_SIZE) * TAR_BLOCK_SIZE;
    }

    if (onProgress && totalBytes > 0) {
        onProgress(totalBytes, totalBytes);  // 100% at end
    }
}

}  // namespace updater
