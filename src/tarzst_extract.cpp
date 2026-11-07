#include "tarzst_extract.hpp"
#include "utils.hpp"
#include <zstd.h>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string_view>
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

// PAX extended header (POSIX ustar typeflag 'x' / 'g'): records are
//   <decimal-length> <space> keyword=value\n
// length counts the whole line including trailing newline.
static std::string paxUnescape(std::string_view v) {
    std::string out;
    out.reserve(v.size());
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i] == '\\' && i + 1 < v.size()) {
            switch (v[i + 1]) {
                case '\\':
                    out += '\\';
                    ++i;
                    break;
                case 'n':
                    out += '\n';
                    ++i;
                    break;
                case 'r':
                    out += '\r';
                    ++i;
                    break;
                default:
                    out += v[i];
                    break;
            }
        } else {
            out += v[i];
        }
    }
    return out;
}

// Returns UTF-8 path from PAX `path=` if present (GNU/libarchive use this for Unicode names).
static std::optional<std::string> parsePaxPathField(const uint8_t* data, size_t size) {
    const char* p = reinterpret_cast<const char*>(data);
    const char* end = p + size;
    std::optional<std::string> best;
    while (p < end) {
        while (p < end && (*p == '\0' || *p == '\n')) ++p;
        if (p >= end) break;

        // Record length (decimal)
        size_t reclen = 0;
        const char* q = p;
        while (q < end && static_cast<unsigned char>(*q) >= '0' && static_cast<unsigned char>(*q) <= '9') {
            reclen = reclen * 10 + static_cast<size_t>(*q - '0');
            ++q;
        }
        if (q == p || reclen == 0 || static_cast<size_t>(end - p) < reclen) break;

        const char* lineEnd = p + reclen;

        while (q < lineEnd && std::isspace(static_cast<unsigned char>(*q))) ++q;
        const char* eq = static_cast<const char*>(std::memchr(q, '=', static_cast<size_t>(lineEnd - q)));
        if (eq && eq < lineEnd) {
            std::string_view keyword(q, static_cast<size_t>(eq - q));
            while (!keyword.empty() && std::isspace(static_cast<unsigned char>(keyword.back()))) {
                keyword.remove_suffix(1);
            }
            if (keyword == "path") {
                std::string_view value(eq + 1, static_cast<size_t>(lineEnd - eq - 1));
                if (!value.empty() && value.back() == '\n') value.remove_suffix(1);
                best = paxUnescape(value);
            }
        }
        p = lineEnd;
    }
    return best;
}

// GNU long name / long link: payload is null-terminated path (typeflag 'L' or 'K').
static std::string gnuLongNameFromData(const uint8_t* data, size_t size) {
    size_t n = 0;
    while (n < size && data[n] != '\0') ++n;
    return std::string(reinterpret_cast<const char*>(data), n);
}

}  // namespace

void extractTarZstToDir(const std::string& tarzstPath, const std::string& destDir,
                        ExtractProgressFn onProgress) {
    // 1. Read compressed file（path 为 UTF-8；MSVC 下需用 path 打开以支持中文路径）
    std::ifstream ifs(std::filesystem::u8path(tarzstPath), std::ios::binary | std::ios::ate);
    if (!ifs) {
        throw std::runtime_error("Failed to open: " + tarzstPath);
    }
    std::streamsize fileSize = ifs.tellg();
    if (fileSize <= 0) throw std::runtime_error("Empty or unreadable archive: " + tarzstPath);
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
        tarData.resize(result);
    } else {
        // Streaming decompression for unknown size
        ZSTD_DCtx* dctx = ZSTD_createDCtx();
        if (!dctx) {
            throw std::runtime_error("ZSTD_createDCtx failed");
        }
        const size_t bufSize = 1024 * 1024;  // 1MB chunks
        std::vector<uint8_t> outBuf(bufSize);
        ZSTD_inBuffer inBuf = {compressed.data(), compressedSize, 0};
        size_t remaining = 1;
        while (inBuf.pos < inBuf.size) {
            ZSTD_outBuffer outBufStruct = {outBuf.data(), bufSize, 0};
            remaining = ZSTD_decompressStream(dctx, &outBufStruct, &inBuf);
            if (ZSTD_isError(remaining)) {
                ZSTD_freeDCtx(dctx);
                throw std::runtime_error(std::string("Zstd stream decompress failed: ") + ZSTD_getErrorName(remaining));
            }
            tarData.insert(tarData.end(), outBuf.data(), outBuf.data() + outBufStruct.pos);
        }
        ZSTD_freeDCtx(dctx);
        if (remaining != 0) throw std::runtime_error("Truncated zstd frame");
    }

    if (tarData.size() < TAR_BLOCK_SIZE * 2) throw std::runtime_error("Invalid tar archive");
    const uint8_t* p = tarData.data();
    const uint8_t* end = tarData.data() + tarData.size();
    const size_t totalBytes = tarData.size();

    std::filesystem::path destBase = std::filesystem::u8path(destDir);

    // GNU / POSIX PAX: UTF-8 path 往往在扩展头 (typeflag 'x') 或 GNU 长文件名 ('L') 中，
    // 仅读 ustar name/prefix 会得到错误字节序列，在 Windows 上无法创建文件。
    std::optional<std::string> pendingPaxPath;
    std::optional<std::string> pendingGnuLongName;

    bool reachedEnd = false;
    while (static_cast<size_t>(end - p) >= TAR_BLOCK_SIZE * 2) {
        if (isZeroBlock(p)) {
            p += TAR_BLOCK_SIZE;
            if (p + TAR_BLOCK_SIZE <= end && isZeroBlock(p)) {
                reachedEnd = true;
                break;
            }
            continue;
        }

        const auto* h = reinterpret_cast<const UstarHeader*>(p);
        size_t entrySize = parseOctal(h->size, 12);
        const char typeflag = h->typeflag;
        const size_t dataBlocks = entrySize / TAR_BLOCK_SIZE + (entrySize % TAR_BLOCK_SIZE != 0);
        if (dataBlocks > static_cast<size_t>(end - p) / TAR_BLOCK_SIZE - 1) {
            throw std::runtime_error("Truncated tar entry");
        }

        // PAX extended header (local 'x' / global 'g')
        if (typeflag == 'x' || typeflag == 'g') {
            const uint8_t* dataStart = p + TAR_BLOCK_SIZE;
            if (typeflag == 'x') {
                // 多个连续 x 块会合并；仅当本块含 path= 时更新，避免后续无 path 的块清掉已有路径。
                if (auto opt = parsePaxPathField(dataStart, entrySize)) {
                    pendingPaxPath = std::move(opt);
                }
            }
            size_t blockCount = 1 + (entrySize + TAR_BLOCK_SIZE - 1) / TAR_BLOCK_SIZE;
            size_t bytesProcessed = static_cast<size_t>(p - tarData.data()) + blockCount * TAR_BLOCK_SIZE;
            if (onProgress && totalBytes > 0) {
                onProgress(std::min(bytesProcessed, totalBytes), totalBytes);
            }
            p += TAR_BLOCK_SIZE + ((entrySize + TAR_BLOCK_SIZE - 1) / TAR_BLOCK_SIZE) * TAR_BLOCK_SIZE;
            continue;
        }

        // GNU long name ('L') / long link ('K')
        if (typeflag == 'L' || typeflag == 'K') {
            const uint8_t* dataStart = p + TAR_BLOCK_SIZE;
            if (typeflag == 'L') {
                pendingGnuLongName = gnuLongNameFromData(dataStart, entrySize);
            }
            size_t blockCount = 1 + (entrySize + TAR_BLOCK_SIZE - 1) / TAR_BLOCK_SIZE;
            size_t bytesProcessed = static_cast<size_t>(p - tarData.data()) + blockCount * TAR_BLOCK_SIZE;
            if (onProgress && totalBytes > 0) {
                onProgress(std::min(bytesProcessed, totalBytes), totalBytes);
            }
            p += TAR_BLOCK_SIZE + ((entrySize + TAR_BLOCK_SIZE - 1) / TAR_BLOCK_SIZE) * TAR_BLOCK_SIZE;
            continue;
        }

        size_t nameLen = 0;
        while (nameLen < 100 && h->name[nameLen] != '\0') ++nameLen;
        size_t prefixLen = 0;
        while (prefixLen < 155 && h->prefix[prefixLen] != '\0') ++prefixLen;

        std::string pathFromUstar = buildPath(h->prefix, prefixLen, h->name, nameLen);

        std::string pathStr;
        if (pendingPaxPath) {
            pathStr = std::move(*pendingPaxPath);
            pendingPaxPath.reset();
        } else if (pendingGnuLongName) {
            pathStr = std::move(*pendingGnuLongName);
            pendingGnuLongName.reset();
        } else {
            pathStr = pathFromUstar;
        }

        if (pathStr.empty()) {
            p += TAR_BLOCK_SIZE + ((entrySize + TAR_BLOCK_SIZE - 1) / TAR_BLOCK_SIZE) * TAR_BLOCK_SIZE;
            continue;
        }

        std::replace(pathStr.begin(), pathStr.end(), '\\', '/');
        const auto relative = safeRelativePath(std::filesystem::u8path(pathStr));
        const auto destination = destBase / relative;

        if (typeflag == '5') {
            std::filesystem::create_directories(destination);
        } else if (typeflag == '0' || typeflag == '\0') {
            std::filesystem::create_directories(destination.parent_path());
            std::ofstream out(destination, std::ios::binary);
            out.write(reinterpret_cast<const char*>(p + TAR_BLOCK_SIZE),
                      static_cast<std::streamsize>(entrySize));
            out.close();
            if (!out) throw std::runtime_error("Failed to extract: " + pathStr);
#ifndef _WIN32
            std::filesystem::permissions(destination,
                static_cast<std::filesystem::perms>(parseOctal(h->mode, sizeof(h->mode)) & 0777));
#endif
        } else {
            throw std::runtime_error("Unsupported tar entry type: " + pathStr);
        }

        size_t blockCount = 1 + (entrySize + TAR_BLOCK_SIZE - 1) / TAR_BLOCK_SIZE;
        size_t bytesProcessed = static_cast<size_t>(p - tarData.data()) + blockCount * TAR_BLOCK_SIZE;
        if (onProgress && totalBytes > 0) {
            onProgress(std::min(bytesProcessed, totalBytes), totalBytes);
        }

        p += TAR_BLOCK_SIZE + ((entrySize + TAR_BLOCK_SIZE - 1) / TAR_BLOCK_SIZE) * TAR_BLOCK_SIZE;
    }

    if (!reachedEnd) throw std::runtime_error("Missing tar end marker");
    if (onProgress && totalBytes > 0) {
        onProgress(totalBytes, totalBytes);  // 100% at end
    }
}

}  // namespace updater
