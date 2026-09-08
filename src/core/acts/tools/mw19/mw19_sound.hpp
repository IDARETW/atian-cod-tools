#pragma once
#include "mw19_payload.hpp"
#include <span>

namespace tool::mw19::sound {
    struct Entry {
        uint32_t key{}, size{}, seekBytes{}, frames{}, hybridPcmBytes{};
        uint64_t offset{};
        uint32_t rate{};
        uint8_t channels{}, looping{}, format{};
    };
    // Reader offsets are relative to the start of the SAB container. Unlike a
    // process address, offset zero is valid. Construction reads only metadata.
    class Bank {
        schema::MemoryReader read;
        uint64_t fileSize{}, indexOffset{}, checksumOffset{};
        size_t metadataBytes{};
        std::vector<Entry> entries;

      public:
        Bank(schema::MemoryReader readAt, uint64_t size, size_t maxIndexBytes = 16 * 1024 * 1024);
        const std::vector<Entry>& Entries() const { return entries; }
        const Entry* Find(uint32_t key) const;
        schema::Payload ExtractFlac(uint32_t key, size_t maxBytes = 128 * 1024 * 1024) const;
    };
    schema::Json Describe(const Entry& entry);
    // Reconstruct STREAMINFO from validated FLAC frame headers and CRCs. The
    // original encoded frames remain unchanged. This is framing validation,
    // not a full entropy decoder or PCM checksum verification.
    schema::Payload RestoreFlac(std::span<const uint8_t> frames, const Entry& entry, size_t maxBytes);
} // namespace tool::mw19::sound
