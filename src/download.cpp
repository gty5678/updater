#include "download.hpp"
#include "utils.hpp"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <curl/curl.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace updater {

namespace {

/** 与常见浏览器一致，便于 CDN/服务器同等调度；末尾保留本程序标识。 */
static const char kDownloadUserAgent[] =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/131.0.0.0 Safari/537.36 DarkEyeUpdater/1.0";

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
        /* 优先 HTTP/3，库未编 QUIC 或网络不支持时由 libcurl 回落至 HTTP/2 / 1.1（HTTP/2 需编进 nghttp2） */
        curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_3);
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
