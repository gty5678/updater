#include "download.hpp"
#include "utils.hpp"
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#include <algorithm>
#include <chrono>
#include <thread>

#pragma comment(lib, "winhttp.lib")

// 与较新 Windows SDK winhttp.h 一致；旧头文件可能缺少 HTTP/3 常量
#ifndef WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL
#define WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL 133
#endif
#ifndef WINHTTP_PROTOCOL_FLAG_HTTP2
#define WINHTTP_PROTOCOL_FLAG_HTTP2 0x1
#endif
#ifndef WINHTTP_PROTOCOL_FLAG_HTTP3
#define WINHTTP_PROTOCOL_FLAG_HTTP3 0x2
#endif

namespace updater {

constexpr DWORD kDownloadReadBufferBytes = 256 * 1024;

static std::wstring utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) return L"";

    int size = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (size <= 0) return L"";

    std::wstring result(size, 0);
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &result[0], size);
    result.resize(size - 1);
    return result;
}

std::vector<uint8_t> downloadBytes(const std::string& url,
                                   int timeoutSeconds,
                                   int retries,
                                   DownloadProgressFn onProgress) {
    int attempts = std::max(1, retries);
    std::wstring urlW = utf8ToWide(url);

    for (int i = 0; i < attempts; ++i) {
        std::vector<uint8_t> data;
        URL_COMPONENTS urlComp{};
        urlComp.dwStructSize = sizeof(urlComp);
        wchar_t hostBuf[256] = {};
        wchar_t pathBuf[2048] = {};
        urlComp.lpszHostName = hostBuf;
        urlComp.dwHostNameLength = 256;
        urlComp.lpszUrlPath = pathBuf;
        urlComp.dwUrlPathLength = 2048;

        if (!WinHttpCrackUrl(urlW.c_str(), 0, 0, &urlComp)) {
            if (i < attempts - 1) {
                int sleepSec = std::min(8, 1 + i * 2);
                log("Download failed (attempt " + std::to_string(i + 1) + "/" + std::to_string(attempts) +
                    "): URL parse error; retry in " + std::to_string(sleepSec) + "s...");
                std::this_thread::sleep_for(std::chrono::seconds(sleepSec));
            } else {
                throw std::runtime_error("Download failed: invalid URL " + url);
            }
            continue;
        }

        INTERNET_PORT port = urlComp.nPort;
        if (port == 0) {
            port = (urlComp.nScheme == INTERNET_SCHEME_HTTPS) ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
        }

        HINTERNET hSession = WinHttpOpen(
            L"DarkEye-Updater/1.0",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS,
            0
        );
        if (!hSession) {
            if (i < attempts - 1) {
                int sleepSec = std::min(8, 1 + i * 2);
                log("Download failed (attempt " + std::to_string(i + 1) + "/" + std::to_string(attempts) +
                    "): WinHttpOpen failed; retry in " + std::to_string(sleepSec) + "s...");
                std::this_thread::sleep_for(std::chrono::seconds(sleepSec));
            } else {
                throw std::runtime_error("Download failed: WinHttpOpen failed");
            }
            continue;
        }

        DWORD timeoutMs = static_cast<DWORD>(timeoutSeconds) * 1000;
        WinHttpSetOption(hSession, WINHTTP_OPTION_CONNECT_TIMEOUT, &timeoutMs, sizeof(timeoutMs));
        WinHttpSetOption(hSession, WINHTTP_OPTION_SEND_TIMEOUT, &timeoutMs, sizeof(timeoutMs));
        WinHttpSetOption(hSession, WINHTTP_OPTION_RECEIVE_TIMEOUT, &timeoutMs, sizeof(timeoutMs));

        DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
        WinHttpSetOption(hSession, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));

        if (urlComp.nScheme == INTERNET_SCHEME_HTTPS) {
            DWORD protocols = WINHTTP_PROTOCOL_FLAG_HTTP2 | WINHTTP_PROTOCOL_FLAG_HTTP3;
            WinHttpSetOption(hSession, WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL, &protocols, sizeof(protocols));
        }

        HINTERNET hConnect = WinHttpConnect(hSession, urlComp.lpszHostName, port, 0);
        if (!hConnect) {
            WinHttpCloseHandle(hSession);
            if (i < attempts - 1) {
                int sleepSec = std::min(8, 1 + i * 2);
                log("Download failed (attempt " + std::to_string(i + 1) + "/" + std::to_string(attempts) +
                    "): WinHttpConnect failed; retry in " + std::to_string(sleepSec) + "s...");
                std::this_thread::sleep_for(std::chrono::seconds(sleepSec));
            } else {
                throw std::runtime_error("Download failed: WinHttpConnect failed");
            }
            continue;
        }

        LPCWSTR verb = L"GET";
        DWORD flags = (urlComp.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;

        HINTERNET hRequest = WinHttpOpenRequest(
            hConnect,
            verb,
            urlComp.lpszUrlPath[0] ? urlComp.lpszUrlPath : L"/",
            nullptr,
            WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES,
            flags
        );
        if (!hRequest) {
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            if (i < attempts - 1) {
                int sleepSec = std::min(8, 1 + i * 2);
                log("Download failed (attempt " + std::to_string(i + 1) + "/" + std::to_string(attempts) +
                    "): WinHttpOpenRequest failed; retry in " + std::to_string(sleepSec) + "s...");
                std::this_thread::sleep_for(std::chrono::seconds(sleepSec));
            } else {
                throw std::runtime_error("Download failed: WinHttpOpenRequest failed");
            }
            continue;
        }

        BOOL sendOk = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                        WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
        if (!sendOk) {
            WinHttpCloseHandle(hRequest);
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            if (i < attempts - 1) {
                int sleepSec = std::min(8, 1 + i * 2);
                log("Download failed (attempt " + std::to_string(i + 1) + "/" + std::to_string(attempts) +
                    "): WinHttpSendRequest failed; retry in " + std::to_string(sleepSec) + "s...");
                std::this_thread::sleep_for(std::chrono::seconds(sleepSec));
            } else {
                throw std::runtime_error("Download failed: WinHttpSendRequest failed");
            }
            continue;
        }

        BOOL recvOk = WinHttpReceiveResponse(hRequest, nullptr);
        if (!recvOk) {
            WinHttpCloseHandle(hRequest);
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            if (i < attempts - 1) {
                int sleepSec = std::min(8, 1 + i * 2);
                log("Download failed (attempt " + std::to_string(i + 1) + "/" + std::to_string(attempts) +
                    "): WinHttpReceiveResponse failed; retry in " + std::to_string(sleepSec) + "s...");
                std::this_thread::sleep_for(std::chrono::seconds(sleepSec));
            } else {
                throw std::runtime_error("Download failed: WinHttpReceiveResponse failed");
            }
            continue;
        }

        DWORD statusCode = 0;
        DWORD statusSize = sizeof(statusCode);
        WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);

        if (statusCode < 200 || statusCode >= 300) {
            WinHttpCloseHandle(hRequest);
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            if (i < attempts - 1) {
                int sleepSec = std::min(8, 1 + i * 2);
                log("Download failed (attempt " + std::to_string(i + 1) + "/" + std::to_string(attempts) +
                    "): HTTP " + std::to_string(statusCode) + "; retry in " + std::to_string(sleepSec) + "s...");
                std::this_thread::sleep_for(std::chrono::seconds(sleepSec));
            } else {
                throw std::runtime_error("Download failed after " + std::to_string(attempts) + " attempts: HTTP " + std::to_string(statusCode));
            }
            continue;
        }

        DWORD contentLength = 0;
        DWORD clSize = sizeof(contentLength);
        if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &contentLength, &clSize, WINHTTP_NO_HEADER_INDEX)) {
            /* got Content-Length */
        } else {
            contentLength = 0;
        }
        size_t totalBytes = static_cast<size_t>(contentLength);

        for (;;) {
            DWORD bytesAvailable = 0;
            if (!WinHttpQueryDataAvailable(hRequest, &bytesAvailable) || bytesAvailable == 0) {
                break;
            }

            const DWORD toRead = (bytesAvailable > kDownloadReadBufferBytes)
                                       ? kDownloadReadBufferBytes
                                       : bytesAvailable;
            const size_t oldSize = data.size();
            data.resize(oldSize + toRead);
            DWORD bytesRead = 0;
            if (!WinHttpReadData(hRequest, data.data() + oldSize, toRead, &bytesRead)) {
                data.resize(oldSize);
                break;
            }
            if (bytesRead == 0) {
                data.resize(oldSize);
                break;
            }
            data.resize(oldSize + bytesRead);

            if (onProgress) {
                onProgress(data.size(), totalBytes);
            }
        }

        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);

        return data;
    }

    throw std::runtime_error("Download failed");
}

}  // namespace updater
