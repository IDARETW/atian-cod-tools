#pragma once
#include "mw19_payload.hpp"

namespace tool::mw19::schema {
    Payload
    ExportReplayImage(const MemoryReader& read, uint64_t address, size_t maxBytes, const PackageReader& packages = {});
}
