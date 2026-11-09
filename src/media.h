#pragma once
#include "ndi.h"
namespace nr {
int runWorker(const fs::path &jobFile);
void inspectInterrupted(const fs::path &, Logger &, std::stop_token);
int probeEncoders(const fs::path &output);
} // namespace nr
