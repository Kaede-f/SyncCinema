#pragma once

#include <string>
#include <chrono>

using Clock = std::chrono::steady_clock;

void runClient(const std::string& mediaSource, const std::string& serverIp);
