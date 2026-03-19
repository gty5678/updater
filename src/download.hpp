#pragma once

#include <string>
#include <vector>

namespace updater {

std::vector<uint8_t> downloadBytes(const std::string& url,
                                  int timeoutSeconds = 20,
                                  int retries = 3);

}  // namespace updater
