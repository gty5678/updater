#include "download.hpp"
#include "resume.hpp"
#include "utils.hpp"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <curl/curl.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>
#include <memory>
#include <exception>
#include <cctype>
#include <stdexcept>
#include <thread>

namespace updater {

namespace {

constexpr char kDownloadUserAgent[] = "Updater/1.0";

std::once_flag gCurlInitOnce;

void curlGlobalInitOnce() {
    std::call_once(gCurlInitOnce, []() {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0) {
            throw std::runtime_error("curl_global_init failed");
        }
    });
}

size_t writeCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* data = static_cast<std::vector<uint8_t>*>(userdata);
    const size_t n = size * nmemb;
    const size_t oldSize = data->size();
    data->resize(oldSize + n);
    if (n > 0) {
        std::memcpy(data->data() + oldSize, ptr, n);
    }
    return n;
}

struct ProgressState {
    DownloadProgressFn onProgress;
};

int xferinfoCallback(void* clientp,
                     curl_off_t dltotal,
                     curl_off_t dlnow,
                     curl_off_t /*ultotal*/,
                     curl_off_t /*ulnow*/) {
    auto* st = static_cast<ProgressState*>(clientp);
    if (!st->onProgress) {
        return 0;
    }
    const size_t total = (dltotal > 0) ? static_cast<size_t>(dltotal) : 0;
    st->onProgress(static_cast<size_t>(dlnow), total);
    return 0;
}

}  // namespace

namespace {
struct FileTransfer {
    ResumableFile& file;
    std::map<std::string, std::string> headers;
    long status = 0;
    bool body = false;
    bool headersDone = false;
    std::exception_ptr error;
    explicit FileTransfer(ResumableFile& output) : file(output) {}
};
size_t fileHeader(char* ptr, size_t size, size_t count, void* context) {
    auto& state = *static_cast<FileTransfer*>(context);
    const auto length = size * count;
    try {
        const std::string line(ptr, length);
        if (line.rfind("HTTP/", 0) == 0) {
            const auto space = line.find(' ');
            state.status = std::stol(line.substr(space + 1));
            state.headers.clear();
            state.body = false;
            state.headersDone = false;
        } else if (line == "\r\n" || line == "\n") {
            if (!state.headersDone && state.status >= 200 && (state.status < 300 || state.status >= 400)) {
                state.headersDone = true;
                state.file.begin(state.status, state.headers);
                state.body = state.status != 416;
            }
        } else {
            const auto colon = line.find(':');
            if (colon != std::string::npos) {
                auto name = line.substr(0, colon);
                for (auto& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                state.headers[name] = trim(line.substr(colon + 1));
            }
        }
        return length;
    } catch (...) { state.error = std::current_exception(); return 0; }
}
size_t fileWrite(char* ptr, size_t size, size_t count, void* context) {
    auto& state = *static_cast<FileTransfer*>(context);
    try {
        if (state.body) state.file.write(ptr, size * count);
        return size * count;
    } catch (...) { state.error = std::current_exception(); return 0; }
}
}

void downloadFile(const std::string& url, const std::string& path,
                  const std::string& sha, int timeoutSeconds, int retries,
                  DownloadProgressFn onProgress) {
    curlGlobalInitOnce();
    const int attempts = std::max(1, retries);
    for (int i = 0; i < attempts; ++i) {
        std::string error;
        try {
            ResumableFile file(url, path, sha, onProgress);
            FileTransfer state{file};
            std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
            if (!curl) throw std::runtime_error("curl_easy_init failed");
            auto* handle = curl.get();
            curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
            curl_easy_setopt(handle, CURLOPT_USERAGENT, kDownloadUserAgent);
            curl_easy_setopt(handle, CURLOPT_PROTOCOLS_STR, "http,https");
            curl_easy_setopt(handle, CURLOPT_REDIR_PROTOCOLS_STR, url.rfind("https://", 0) == 0 ? "https" : "http,https");
            curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(handle, CURLOPT_MAXREDIRS, 20L);
            curl_easy_setopt(handle, CURLOPT_SUPPRESS_CONNECT_HEADERS, 1L);
            curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
            curl_easy_setopt(handle, CURLOPT_TIMEOUT, static_cast<long>(std::max(1, timeoutSeconds)));
            curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, static_cast<long>(std::max(1, timeoutSeconds)));
            curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, fileHeader);
            curl_easy_setopt(handle, CURLOPT_HEADERDATA, &state);
            curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, fileWrite);
            curl_easy_setopt(handle, CURLOPT_WRITEDATA, &state);
            curl_slist* rawHeaders = curl_slist_append(nullptr, "Accept-Encoding: identity");
            if (file.offset()) {
                const auto range = std::to_string(file.offset()) + "-";
                curl_easy_setopt(handle, CURLOPT_RANGE, range.c_str());
                if (!file.validator().empty())
                    rawHeaders = curl_slist_append(rawHeaders, ("If-Range: " + file.validator()).c_str());
            }
            std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(rawHeaders, curl_slist_free_all);
            curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers.get());
            const auto result = curl_easy_perform(handle);
            if (state.error) std::rethrow_exception(state.error);
            if (result != CURLE_OK) throw std::runtime_error(curl_easy_strerror(result));
            file.finish();
            return;
        } catch (const std::exception& e) { error = e.what(); }
        progressBarEnd();
        if (i + 1 == attempts)
            throw std::runtime_error("Download failed after " + std::to_string(attempts) + " attempts: " + error);
        log("Download failed (attempt " + std::to_string(i + 1) + "/" + std::to_string(attempts) +
            "): " + error + "; partial package retained; retrying...");
        std::this_thread::sleep_for(std::chrono::seconds(std::min(8, 1 + i * 2)));
    }
}

std::vector<uint8_t> downloadBytes(const std::string& url,
                                   int timeoutSeconds,
                                   int retries,
                                   DownloadProgressFn onProgress) {
    curlGlobalInitOnce();

    int attempts = std::max(1, retries);
    const long timeoutSec = std::max(1, timeoutSeconds);

    for (int i = 0; i < attempts; ++i) {
        std::vector<uint8_t> data;
        CURL* curl = curl_easy_init();
        if (!curl) {
            if (i < attempts - 1) {
                int sleepSec = std::min(8, 1 + i * 2);
                log("Download failed (attempt " + std::to_string(i + 1) + "/" + std::to_string(attempts) +
                    "): curl_easy_init failed; retry in " + std::to_string(sleepSec) + "s...");
                std::this_thread::sleep_for(std::chrono::seconds(sleepSec));
            } else {
                throw std::runtime_error("Download failed: curl_easy_init failed");
            }
            continue;
        }

        ProgressState progressSt{onProgress};

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_USERAGENT, kDownloadUserAgent);
        curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
        curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR,
                         url.rfind("https://", 0) == 0 ? "https" : "http,https");
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 20L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeoutSec);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, timeoutSec);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &data);
        if (onProgress) {
            curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
            curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, xferinfoCallback);
            curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &progressSt);
        } else {
            curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
        }

        CURLcode res = curl_easy_perform(curl);
        long httpCode = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
        curl_easy_cleanup(curl);

        if (res == CURLE_OK && httpCode >= 200 && httpCode < 300) {
            return data;
        }

        if (i < attempts - 1) {
            int sleepSec = std::min(8, 1 + i * 2);
            std::string detail = (res != CURLE_OK) ? std::string(curl_easy_strerror(res)) : ("HTTP " + std::to_string(httpCode));
            log("Download failed (attempt " + std::to_string(i + 1) + "/" + std::to_string(attempts) + "): " + detail +
                "; retry in " + std::to_string(sleepSec) + "s...");
            std::this_thread::sleep_for(std::chrono::seconds(sleepSec));
        } else {
            if (res != CURLE_OK) {
                throw std::runtime_error(std::string("Download failed after ") + std::to_string(attempts) +
                                         " attempts: " + curl_easy_strerror(res));
            }
            throw std::runtime_error("Download failed after " + std::to_string(attempts) + " attempts: HTTP " +
                                     std::to_string(httpCode));
        }
    }

    throw std::runtime_error("Download failed");
}

}  // namespace updater
