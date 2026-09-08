#pragma once
#include "mw19_payload.hpp"
#include <span>

namespace tool::mw19::schema {
    // A base-surface geometry view, not a complete XModel. Skinning, materials,
    // morphs and subdivision metadata remain in the separate structured dump.
    struct GeometryPayload {
        Payload payload;
        size_t surfaces{}, vertices{}, triangles{};
    };
    // sharedData is the exact buffer already retrieved by ExportPayload for
    // this XModelSurfs root. No second package decode or runtime handle lookup.
    GeometryPayload ExportReplayGeometry(
        const MemoryReader& read, uint64_t address, std::span<const uint8_t> sharedData,
        size_t maxBytes = 128 * 1024 * 1024
    );
} // namespace tool::mw19::schema
