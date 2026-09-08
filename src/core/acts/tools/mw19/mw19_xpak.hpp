#pragma once
#include "mw19_schema.hpp"
#include <span>
#include <optional>
#include <stdexcept>

namespace tool::mw19::xpak {
    class DecoderUnavailable : public std::runtime_error {
      public:
        using std::runtime_error::runtime_error;
    };
    using ReadAt = schema::MemoryReader;
    using OodleDecoder = std::function<bool(std::span<const uint8_t>, std::span<uint8_t>)>;
    struct Entry {
        uint64_t key, offset, size;
        bool compressed;
    };
    struct Limits {
        size_t maxIndexBytes{ 64 * 1024 * 1024 };
        size_t maxPayloadBytes{ 128 * 1024 * 1024 };
        size_t maxMetadataBytes{ 65536 };
    };
    std::vector<uint8_t> DecodeBlocks(
        std::span<const uint8_t> input, size_t outputSize, const OodleDecoder& oodle = {},
        size_t maxBytes = 128 * 1024 * 1024
    );
    class Archive {
        ReadAt read;
        uint64_t fileSize{}, dataOffset{}, dataSize{}, metaOffset{}, metaSize{}, metaCount{};
        Limits limits;
        std::vector<Entry> entries;
        std::vector<uint8_t> Read(uint64_t offset, size_t length) const;

      public:
        Archive(ReadAt read, uint64_t fileSize, Limits limits = {});
        static Archive Open(const std::filesystem::path& path, Limits limits = {});
        const std::vector<Entry>& Entries() const { return entries; }
        std::optional<Entry> Find(uint64_t key) const;
        std::optional<std::string> Metadata(uint64_t key) const;
        std::vector<uint8_t> Extract(uint64_t key, size_t outputSize, const OodleDecoder& oodle = {}) const;
    };
} // namespace tool::mw19::xpak
