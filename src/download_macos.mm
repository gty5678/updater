#include "download.hpp"
#include "utils.hpp"
#import <Foundation/Foundation.h>
#include <algorithm>
#include <chrono>
#include <exception>
#include <stdexcept>
#include <thread>

// A serial delegate queue owns all transfer state until completion signals the caller.
@interface UpdaterDownload : NSObject <NSURLSessionDataDelegate> {
@public
    std::vector<uint8_t> bytes;
    updater::DownloadProgressFn progress;
    std::exception_ptr callbackError;
    dispatch_semaphore_t completed;
    NSString *failure;
    bool requireHTTPS;
    unsigned redirects;
}
@end

@implementation UpdaterDownload
- (void)URLSession:(NSURLSession *)session task:(NSURLSessionTask *)task
 willPerformHTTPRedirection:(NSHTTPURLResponse *)response newRequest:(NSURLRequest *)request
 completionHandler:(void (^)(NSURLRequest *))handler {
    NSString *scheme = request.URL.scheme.lowercaseString;
    if (++redirects > 20 || !([scheme isEqualToString:@"https"] ||
                              (!requireHTTPS && [scheme isEqualToString:@"http"]))) {
        failure = @"Unsafe redirect or too many redirects";
        handler(nil);
    } else {
        handler(request);
    }
}
- (void)URLSession:(NSURLSession *)session dataTask:(NSURLSessionDataTask *)task
    didReceiveData:(NSData *)data {
    if (callbackError) return;
    try {
        const auto *start = static_cast<const uint8_t *>(data.bytes);
        if (data.length) bytes.insert(bytes.end(), start, start + data.length);
        if (progress) {
            auto total = task.response.expectedContentLength;
            progress(bytes.size(), total > 0 ? static_cast<size_t>(total) : 0);
        }
    } catch (...) {
        callbackError = std::current_exception();
        [task cancel];
    }
}
- (void)URLSession:(NSURLSession *)session task:(NSURLSessionTask *)task
    didCompleteWithError:(NSError *)error {
    if (!failure && error) {
        failure = [NSString stringWithFormat:@"%@ (%@ %ld)", error.localizedDescription,
                   error.domain, (long)error.code];
    }
    if (!failure) {
        auto response = (NSHTTPURLResponse *)task.response;
        if (![response isKindOfClass:[NSHTTPURLResponse class]] ||
            response.statusCode < 200 || response.statusCode >= 300) {
            failure = [NSString stringWithFormat:@"HTTP %ld", (long)response.statusCode];
        }
    }
    dispatch_semaphore_signal(completed);
}
@end

namespace updater {
std::vector<uint8_t> downloadBytes(const std::string& url, int timeoutSeconds,
                                  int retries, DownloadProgressFn onProgress) {
    @autoreleasepool {
        NSURL *address = [NSURL URLWithString:[NSString stringWithUTF8String:url.c_str()]];
        NSString *scheme = address.scheme.lowercaseString;
        if (!address.host.length || !([scheme isEqualToString:@"http"] ||
                                      [scheme isEqualToString:@"https"])) {
            throw std::runtime_error("Download URL must use HTTP or HTTPS");
        }
        const int attempts = std::max(1, retries);
        const int timeout = std::max(1, timeoutSeconds);
        for (int i = 0; i < attempts; ++i) {
            UpdaterDownload *transfer = [UpdaterDownload new];
            transfer->progress = onProgress;
            transfer->completed = dispatch_semaphore_create(0);
            transfer->requireHTTPS = [scheme isEqualToString:@"https"];
            NSURLSessionConfiguration *config = [NSURLSessionConfiguration ephemeralSessionConfiguration];
            config.timeoutIntervalForRequest = timeout;
            config.timeoutIntervalForResource = timeout;
            config.requestCachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
            config.HTTPAdditionalHeaders = @{@"User-Agent": @"Updater/1.0"};
            NSOperationQueue *queue = [NSOperationQueue new];
            queue.maxConcurrentOperationCount = 1;
            NSURLSession *session = [NSURLSession sessionWithConfiguration:config delegate:transfer delegateQueue:queue];
            NSURLSessionDataTask *task = [session dataTaskWithURL:address];
            [task resume];
            dispatch_semaphore_wait(transfer->completed, DISPATCH_TIME_FOREVER);
            [session finishTasksAndInvalidate];
            if (transfer->callbackError) std::rethrow_exception(transfer->callbackError);
            if (!transfer->failure) return std::move(transfer->bytes);
            std::string detail = transfer->failure.UTF8String;
            if (i + 1 == attempts) {
                throw std::runtime_error("Download failed after " + std::to_string(attempts) + " attempts: " + detail);
            }
            int delay = std::min(8, 1 + i * 2);
            log("Download failed (attempt " + std::to_string(i + 1) + "/" +
                std::to_string(attempts) + "): " + detail + "; retry in " + std::to_string(delay) + "s...");
            std::this_thread::sleep_for(std::chrono::seconds(delay));
        }
    }
    throw std::runtime_error("Download failed");
}
} // namespace updater
