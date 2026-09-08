#pragma once
#include "mw19_schema.hpp"
#include <array>

namespace tool::mw19::linker {
    using schema::Json;
    struct Zone {
        std::vector<uint8_t> body;
        std::array<uint64_t, 11> blocks{};
        Json assets = Json::array();
    };
    // Input records contain pool/name and pool-specific fields. No process pointers
    // or runtime handles are accepted. Layout targets Replay PC xfile 0xff7 only.
    Zone Link(const Json& profile, const Json& assets, size_t limit = 128 * 1024 * 1024);
    // Unsigned IWffc100, IWC block framing, stored or LZ4 blocks. Stock authentication
    // policy is outside this writer; creating a valid container does not sign it.
    std::vector<uint8_t> Pack(const Zone& zone, bool lz4 = true, size_t chunkSize = 0x10000);
} // namespace tool::mw19::linker
