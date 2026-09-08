#ifndef MW19_SCHEMA_STANDALONE
#include <includes.hpp>
#endif
#include "mw19_xpak.hpp"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <lz4.h>

namespace tool::mw19::xpak {
    namespace {
        template<class T>
        T Number(std::span<const uint8_t> bytes, size_t at) {
            if (at > bytes.size() || sizeof(T) > bytes.size() - at)
                throw std::runtime_error("Truncated XPak record");
            T value{};
            std::memcpy(&value, bytes.data() + at, sizeof(value));
            return value;
        }
    } // namespace
    std::vector<uint8_t>
    DecodeBlocks(std::span<const uint8_t> input, size_t outputSize, const OodleDecoder& oodle, size_t maxBytes) {
        if (input.size() > maxBytes || outputSize > maxBytes)
            throw std::runtime_error("XPak payload exceeds byte budget");
        std::vector<uint8_t> output(outputSize);
        std::vector<std::pair<size_t, size_t>> ranges;
        size_t at = 0, commands = 0;
        while (at < input.size()) {
            if (input.size() - at < 128)
                throw std::runtime_error("Truncated XPak command header");
            const auto count = Number<uint32_t>(input, at);
            size_t destination = Number<uint32_t>(input, at + 4);
            if (!count || count > 30 || destination > outputSize)
                throw std::runtime_error("Invalid XPak command header");
            const auto header = at;
            at += 128;
            for (uint32_t i = 0; i < count; ++i) {
                if (++commands > 65536)
                    throw std::runtime_error("XPak command budget exceeded");
                const auto descriptor = Number<uint32_t>(input, header + 8 + i * 4);
                const auto codec = descriptor >> 24;
                const size_t length = descriptor & 0xffffff;
                if (at > input.size() || length > input.size() - at)
                    throw std::runtime_error("Truncated XPak compressed block");
                if (codec != 207) {
                    const size_t decoded = codec ? std::min(outputSize - destination, size_t{ 0x3ffe0 }) : length;
                    if (!length || !decoded || decoded > outputSize - destination)
                        throw std::runtime_error("XPak block exceeds output allocation");
                    const auto source = input.subspan(at, length);
                    auto target = std::span(output).subspan(destination, decoded);
                    bool ok = false;
                    if (!codec) {
                        std::memcpy(target.data(), source.data(), length);
                        ok = true;
                    } else if (codec == 4 || codec == 5) {
                        const auto size = LZ4_decompress_safe(
                            reinterpret_cast<const char*>(source.data()),
                            reinterpret_cast<char*>(target.data()),
                            static_cast<int>(length),
                            static_cast<int>(decoded)
                        );
                        ok = size >= 0 && static_cast<size_t>(size) == decoded;
                    } else if (codec == 6 || codec == 7) {
                        if (!oodle)
                            throw DecoderUnavailable("XPak Oodle decoder is unavailable");
                        ok = oodle(source, target);
                    } else
                        throw std::runtime_error("Unsupported Replay XPak codec: " + std::to_string(codec));
                    if (!ok)
                        throw std::runtime_error("XPak decompression failed or returned an incorrect size");
                    ranges.emplace_back(destination, destination + decoded);
                    destination += decoded;
                }
                at += (length + 3) & ~size_t{ 3 };
                if (at > input.size() && i + 1 != count)
                    throw std::runtime_error("Truncated XPak inter-block alignment");
            }
            at = (at + 127) & ~size_t{ 127 };
        }
        // Destination offsets are explicit and may reorder groups. Refuse
        // holes or overlapping writes instead of silently returning zeroes.
        std::sort(ranges.begin(), ranges.end());
        size_t covered = 0;
        for (const auto& [begin, end] : ranges) {
            if (begin != covered)
                throw std::runtime_error("XPak output has a gap or overlapping blocks");
            covered = end;
        }
        if (covered != outputSize)
            throw std::runtime_error("XPak output is incomplete");
        return output;
    }
    std::vector<uint8_t> Archive::Read(uint64_t offset, size_t length) const {
        if (offset > fileSize || length > fileSize - offset)
            throw std::runtime_error("XPak read outside file");
        std::vector<uint8_t> result(length);
        if (length && !read(offset, result.data(), length))
            throw std::runtime_error("Cannot read XPak file");
        return result;
    }
    Archive::Archive(ReadAt reader, uint64_t size, Limits lim) : read(std::move(reader)), fileSize(size), limits(lim) {
        auto header = Read(0, 800);
        if (Number<uint32_t>(header, 0) != 0x4950414b || Number<uint16_t>(header, 6) != 13)
            throw std::runtime_error("Not a Replay version 13 XPak");
        if (Number<uint64_t>(header, 16) != fileSize)
            throw std::runtime_error("XPak file size mismatch");
        dataOffset = Number<uint64_t>(header, 0x140);
        dataSize = Number<uint64_t>(header, 0x148);
        const auto count = Number<uint64_t>(header, 0x150), indexOffset = Number<uint64_t>(header, 0x158),
                   indexSize = Number<uint64_t>(header, 0x160);
        metaCount = Number<uint64_t>(header, 0x180);
        metaOffset = Number<uint64_t>(header, 0x188);
        metaSize = Number<uint64_t>(header, 0x190);
        auto section = [&](uint64_t offset, uint64_t length) {
            if (offset > fileSize || length > fileSize - offset || (length && offset < 800))
                throw std::runtime_error("XPak section outside file or overlapping header");
        };
        section(dataOffset, dataSize);
        section(indexOffset, indexSize);
        section(metaOffset, metaSize);
        if (indexSize > limits.maxIndexBytes || count > limits.maxIndexBytes / 24 || indexSize != count * 24 ||
            metaCount > count)
            throw std::runtime_error("Invalid XPak index or metadata count");
        const auto index = Read(indexOffset, static_cast<size_t>(indexSize));
        entries.reserve(static_cast<size_t>(count));
        for (size_t i = 0; i < count; ++i) {
            const auto key = Number<uint64_t>(index, i * 24), offset = Number<uint64_t>(index, i * 24 + 8),
                       packed = Number<uint64_t>(index, i * 24 + 16);
            const auto length = packed & 0x7fffffffffffffff;
            if (offset > dataSize || length > dataSize - offset || (!entries.empty() && key <= entries.back().key))
                throw std::runtime_error("Invalid or unsorted XPak entry");
            entries.push_back({ key, dataOffset + offset, length, bool(packed >> 63) });
        }
    }
    Archive Archive::Open(const std::filesystem::path& path, Limits limits) {
        auto file = std::make_shared<std::ifstream>(path, std::ios::binary);
        if (!*file)
            throw std::runtime_error("Cannot open XPak archive");
        return Archive(
            [file](uint64_t offset, void* target, size_t length) {
                if (offset > INT64_MAX || length > INT64_MAX)
                    return false;
                file->clear();
                file->seekg(static_cast<std::streamoff>(offset));
                file->read(static_cast<char*>(target), static_cast<std::streamsize>(length));
                return bool(*file);
            },
            std::filesystem::file_size(path),
            limits
        );
    }
    std::optional<Entry> Archive::Find(uint64_t key) const {
        const auto at =
            std::lower_bound(entries.begin(), entries.end(), key, [](const Entry& e, uint64_t k) { return e.key < k; });
        return at != entries.end() && at->key == key ? std::optional(*at) : std::nullopt;
    }
    std::optional<std::string> Archive::Metadata(uint64_t key) const {
        uint64_t at = 0;
        for (uint64_t i = 0; i < metaCount; ++i) {
            if (at > metaSize || 16 > metaSize - at)
                throw std::runtime_error("Truncated XPak metadata record");
            const auto header = Read(metaOffset + at, 16);
            const auto recordKey = Number<uint64_t>(header, 0), length = Number<uint64_t>(header, 8);
            at += 16;
            if (length > metaSize - at || length > limits.maxMetadataBytes)
                throw std::runtime_error("Invalid XPak metadata length");
            if (recordKey == key) {
                const auto value = Read(metaOffset + at, static_cast<size_t>(length));
                return std::string(value.begin(), value.end());
            }
            at += length;
        }
        return std::nullopt;
    }
    std::vector<uint8_t> Archive::Extract(uint64_t key, size_t outputSize, const OodleDecoder& oodle) const {
        const auto entry = Find(key);
        if (!entry)
            throw std::runtime_error("XPak key was not found");
        if (entry->size > limits.maxPayloadBytes || outputSize > limits.maxPayloadBytes)
            throw std::runtime_error("XPak entry exceeds byte budget");
        auto bytes = Read(entry->offset, static_cast<size_t>(entry->size));
        if (entry->compressed)
            return DecodeBlocks(bytes, outputSize, oodle, limits.maxPayloadBytes);
        if (bytes.size() != outputSize)
            throw std::runtime_error("Raw XPak entry size mismatch");
        return bytes;
    }
} // namespace tool::mw19::xpak
