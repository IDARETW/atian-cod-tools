#ifndef MW19_SCHEMA_STANDALONE
#include <includes.hpp>
#endif
#include "mw19_sound.hpp"
#include <algorithm>
#include <array>
#include <cstring>

namespace tool::mw19::sound {
    namespace {
        template<typename T>
        T Get(std::span<const uint8_t> b, size_t at) {
            if (at > b.size() || sizeof(T) > b.size() - at)
                throw std::runtime_error("Truncated sound-bank metadata");
            T value;
            std::memcpy(&value, b.data() + at, sizeof(value));
            return value;
        }
        void Range(uint64_t offset, uint64_t size, uint64_t end) {
            if (offset > end || size > end - offset)
                throw std::runtime_error("Sound-bank range exceeds file bounds");
        }
        bool Overlaps(uint64_t a, uint64_t an, uint64_t b, uint64_t bn) { return an && bn && a < b + bn && b < a + an; }
        constexpr auto CrcTable() {
            std::array<uint16_t, 256> table{};
            for (size_t i = 0; i < table.size(); ++i) {
                uint16_t crc = static_cast<uint16_t>(i << 8);
                for (int bit = 0; bit < 8; ++bit)
                    crc = static_cast<uint16_t>((crc << 1) ^ ((crc & 0x8000) ? 0x8005 : 0));
                table[i] = crc;
            }
            return table;
        }
        constexpr auto Crc16Table = CrcTable();
        uint16_t Crc16(uint16_t crc, uint8_t byte) {
            return static_cast<uint16_t>((crc << 8) ^ Crc16Table[(crc >> 8) ^ byte]);
        }
        uint8_t Crc8(std::span<const uint8_t> b) {
            uint8_t crc{};
            for (uint8_t byte : b) {
                crc ^= byte;
                for (int bit = 0; bit < 8; ++bit)
                    crc = static_cast<uint8_t>((crc << 1) ^ ((crc & 0x80) ? 7 : 0));
            }
            return crc;
        }
        struct FrameHeader {
            uint64_t number{};
            uint32_t samples{}, rate{}, channels{}, depth{};
            size_t bytes{};
            bool variable{};
        };
        FrameHeader Header(std::span<const uint8_t> data, const Entry& entry, unsigned inheritedDepth) {
            size_t pos{};
            auto byte = [&]() {
                if (pos >= data.size())
                    throw std::runtime_error("Truncated FLAC frame header");
                return data[pos++];
            };
            if (byte() != 0xff)
                throw std::runtime_error("Invalid FLAC frame sync");
            const auto sync = byte();
            if ((sync & 0xfe) != 0xf8)
                throw std::runtime_error("Invalid FLAC blocking strategy or sync");
            FrameHeader h;
            h.variable = sync & 1;
            const auto coding = byte(), layout = byte();
            const unsigned blockCode = coding >> 4, rateCode = coding & 15, channelCode = layout >> 4;
            const unsigned depthCode = (layout >> 1) & 7;
            constexpr unsigned depths[]{ 0, 8, 12, 0, 16, 20, 24, 32 };
            if (!blockCode || rateCode == 15 || channelCode > 10 || (layout & 1) || depthCode == 3)
                throw std::runtime_error("Reserved FLAC frame header field");
            h.channels = channelCode < 8 ? channelCode + 1 : 2;
            h.depth = depthCode ? depths[depthCode] : inheritedDepth;
            if (!h.depth)
                throw schema::PayloadUnavailable(
                    "flac_depth_unavailable",
                    "First FLAC frame has no explicit bit depth"
                );
            // FLAC's UTF-8-like frame/sample number, up to 7 bytes / 36 bits.
            const auto first = byte();
            unsigned leading = 0;
            for (unsigned mask = 0x80; mask && (first & mask); mask >>= 1)
                ++leading;
            if (leading == 1 || leading > 7 || (!h.variable && leading > 6))
                throw std::runtime_error("Invalid FLAC frame number encoding");
            h.number = leading ? first & ((1u << (7 - leading)) - 1) : first;
            for (unsigned i = 1; i < leading; ++i) {
                const auto next = byte();
                if ((next & 0xc0) != 0x80)
                    throw std::runtime_error("Invalid FLAC frame number continuation");
                h.number = (h.number << 6) | (next & 63);
            }
            constexpr uint64_t minimum[]{ 0, 0, 0x80, 0x800, 0x10000, 0x200000, 0x4000000, 0x80000000 };
            if (h.number < minimum[leading] || h.number >= (uint64_t{ 1 } << (h.variable ? 36 : 31)))
                throw std::runtime_error("Overlong or oversized FLAC frame number");
            if (blockCode == 1)
                h.samples = 192;
            else if (blockCode < 6)
                h.samples = 576u << (blockCode - 2);
            else if (blockCode == 6)
                h.samples = byte() + 1u;
            else if (blockCode == 7) {
                const unsigned high = byte();
                h.samples = ((high << 8) | byte()) + 1;
            } else
                h.samples = 1u << blockCode;
            constexpr unsigned rates[]{
                0, 88200, 176400, 192000, 8000, 16000, 22050, 24000, 32000, 44100, 48000, 96000
            };
            if (!rateCode)
                h.rate = entry.rate;
            else if (rateCode < 12)
                h.rate = rates[rateCode];
            else if (rateCode == 12)
                h.rate = byte() * 1000u;
            else {
                const unsigned high = byte();
                h.rate = (high << 8) | byte();
                if (rateCode == 14)
                    h.rate *= 10;
            }
            const auto crc = byte();
            if (crc != Crc8(data.first(pos - 1)))
                throw std::runtime_error("FLAC frame header CRC mismatch");
            if (h.samples > 65535 || h.rate != entry.rate || h.channels != entry.channels ||
                (inheritedDepth && h.depth != inheritedDepth))
                throw std::runtime_error("FLAC frame properties do not match sound-bank metadata");
            h.bytes = pos;
            return h;
        }
        void Big(std::vector<uint8_t>& b, size_t at, uint64_t value, unsigned bytes) {
            for (unsigned i = 0; i < bytes; ++i)
                b[at + bytes - i - 1] = static_cast<uint8_t>(value >> (i * 8));
        }
    } // namespace

    Bank::Bank(schema::MemoryReader readAt, uint64_t size, size_t maxIndexBytes)
        : read(std::move(readAt)), fileSize(size) {
        std::array<uint8_t, 688> header{};
        if (size < header.size() || !read(0, header.data(), header.size()))
            throw std::runtime_error("Cannot read complete sound-bank header");
        // Native Replay validator 0x1B85AB0 checks magic/version/build/entry
        // size and table offsets. Native 0x1B859C0 searches 44-byte records.
        if (Get<uint32_t>(header, 0) != 0x23585532 || Get<uint32_t>(header, 4) != 10 ||
            Get<uint32_t>(header, 8) != 44 || Get<uint32_t>(header, 28) != 16)
            throw std::runtime_error("Expected Replay SAB version 10, build 16, 44-byte entries");
        if (Get<uint64_t>(header, 32) != size)
            throw std::runtime_error("Sound-bank file size mismatch");
        const size_t count = Get<uint32_t>(header, 20);
        if (maxIndexBytes < header.size() || count > (maxIndexBytes - header.size()) / 44)
            throw std::runtime_error("Sound-bank index exceeds metadata budget");
        indexOffset = Get<uint64_t>(header, 40);
        checksumOffset = Get<uint64_t>(header, 48);
        const uint64_t indexBytes = count * 44;
        Range(indexOffset, indexBytes, size);
        // Checksum entries are 16 bytes in this version. Their contents are
        // preserved in a bank dump; this reader does not claim MD5 verification.
        if (Get<uint32_t>(header, 12) != 16)
            throw std::runtime_error("Unexpected sound-bank checksum record size");
        Range(checksumOffset, count * 16, size);
        if (count && (indexOffset < header.size() || checksumOffset < header.size() ||
                      Overlaps(indexOffset, indexBytes, checksumOffset, count * 16)))
            throw std::runtime_error("Overlapping sound-bank metadata");
        if (Get<uint32_t>(header, 679) > size - header.size())
            throw std::runtime_error("Sound-bank asset section exceeds file bounds");
        std::vector<uint8_t> table(static_cast<size_t>(indexBytes));
        if (indexBytes && !read(indexOffset, table.data(), table.size()))
            throw std::runtime_error("Cannot read sound-bank index");
        metadataBytes = header.size() + table.size();
        entries.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            const auto at = i * 44;
            Entry e{ Get<uint32_t>(table, at),      Get<uint32_t>(table, at + 4),  Get<uint32_t>(table, at + 8),
                     Get<uint32_t>(table, at + 12), Get<uint32_t>(table, at + 16), Get<uint64_t>(table, at + 20),
                     Get<uint32_t>(table, at + 28), Get<uint8_t>(table, at + 32),  Get<uint8_t>(table, at + 33),
                     Get<uint8_t>(table, at + 34) };
            const uint64_t extent = static_cast<uint64_t>(e.seekBytes) + e.size + e.hybridPcmBytes;
            Range(e.offset, extent, size);
            if (extent && (e.offset < header.size() || Overlaps(e.offset, extent, indexOffset, indexBytes) ||
                           Overlaps(e.offset, extent, checksumOffset, count * 16)))
                throw std::runtime_error("Sound sample overlaps bank metadata");
            entries.push_back(e);
        }
        // Fastfile-resident SAB indices can arrive in storage order, before
        // runtime bank setup. Sort our lookup copy; the exported SAB and its
        // paired checksum table remain byte-for-byte unchanged.
        std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.key < b.key; });
        for (size_t i = 1; i < entries.size(); ++i)
            if (entries[i - 1].key == entries[i].key)
                throw std::runtime_error("Duplicate sound-bank key: " + std::to_string(entries[i].key));
    }

    const Entry* Bank::Find(uint32_t key) const {
        const auto it =
            std::lower_bound(entries.begin(), entries.end(), key, [](const Entry& e, uint32_t k) { return e.key < k; });
        return it != entries.end() && it->key == key ? &*it : nullptr;
    }

    schema::Payload Bank::ExtractFlac(uint32_t key, size_t maxBytes) const {
        const auto* e = Find(key);
        if (!e)
            throw schema::PayloadUnavailable("sound_key_missing", "Sound key is absent from bank");
        if (e->format != 8 || e->hybridPcmBytes)
            throw schema::PayloadUnavailable(
                "sound_codec_unavailable",
                "Sample is not a standalone Replay FLAC stream"
            );
        if (!e->size || !e->frames)
            throw schema::PayloadUnavailable("sound_sample_empty", "Sound sample is empty");
        if (metadataBytes > maxBytes || static_cast<uint64_t>(e->size) * 2 + 42 > maxBytes - metadataBytes)
            throw std::runtime_error("Sound sample exceeds input/output budget");
        std::vector<uint8_t> data(e->size);
        if (!read(e->offset + e->seekBytes, data.data(), data.size()))
            throw std::runtime_error("Cannot read encoded sound sample");
        return RestoreFlac(data, *e, maxBytes - metadataBytes - data.size());
    }

    schema::Json Describe(const Entry& e) {
        return schema::Json{ { "key", e.key },
                             { "encoded_bytes", e.size },
                             { "seek_bytes", e.seekBytes },
                             { "hybrid_pcm_bytes", e.hybridPcmBytes },
                             { "frames", e.frames },
                             { "sample_rate", e.rate },
                             { "channels", e.channels },
                             { "looping", e.looping != 0 },
                             { "codec", e.format } };
    }

    schema::Payload RestoreFlac(std::span<const uint8_t> data, const Entry& entry, size_t maxBytes) {
        if (entry.format != 8 || entry.hybridPcmBytes)
            throw schema::PayloadUnavailable("sound_codec_unavailable", "Sample requires another sound decoder");
        if (data.size() != entry.size || maxBytes < 42 || data.size() > maxBytes - 42)
            throw std::runtime_error("FLAC size or output budget mismatch");
        if (!entry.frames || !entry.rate || entry.rate > 1048575 || !entry.channels || entry.channels > 8)
            throw std::runtime_error("Invalid sound sample metadata");
        const auto first = Header(data, entry, 0);
        if (first.number)
            throw std::runtime_error("FLAC stream does not begin with frame/sample zero");
        auto current = first;
        size_t frameStart{}, frameCount{};
        uint64_t totalSamples{};
        uint32_t minBlock = 65535, maxBlock{};
        uint16_t crc{};
        for (size_t pos = 0; pos <= data.size(); ++pos) {
            bool boundary = pos == data.size();
            FrameHeader next;
            if (!boundary && pos >= frameStart + current.bytes + 2 && !crc && data[pos] == 0xff) {
                try {
                    next = Header(data.subspan(pos), entry, first.depth);
                    boundary = next.variable == first.variable &&
                               next.number == (first.variable ? totalSamples + current.samples : frameCount + 1);
                } catch (const std::exception&) {
                    // A sync-looking byte sequence inside a coded subframe is
                    // not a frame boundary. The final CRC/count still must pass.
                }
            }
            if (boundary) {
                if (crc || pos < frameStart + current.bytes + 2)
                    throw std::runtime_error("FLAC frame CRC mismatch or truncated stream");
                const bool last = pos == data.size();
                if ((!last && current.samples < 16) ||
                    (!first.variable && current.samples != first.samples && (!last || current.samples > first.samples)))
                    throw std::runtime_error("Invalid FLAC block-size progression");
                if (!last)
                    minBlock = std::min(minBlock, current.samples);
                maxBlock = std::max(maxBlock, current.samples);
                totalSamples += current.samples;
                ++frameCount;
                if (totalSamples > entry.frames)
                    throw std::runtime_error("FLAC stream exceeds declared sample count");
                if (last)
                    break;
                current = next;
                frameStart = pos;
            }
            crc = Crc16(crc, data[pos]);
        }
        if (totalSamples != entry.frames)
            throw std::runtime_error("FLAC frame count does not match sound-bank entry");
        if (frameCount == 1)
            minBlock = std::max(16u, maxBlock);
        maxBlock = std::max(16u, maxBlock);
        schema::Payload result{ ".flac", std::vector<uint8_t>(42, 0), "FLAC" };
        auto& out = result.data;
        std::memcpy(out.data(), "fLaC", 4);
        out[4] = 0x80;
        out[7] = 34;
        Big(out, 8, minBlock, 2);
        Big(out, 10, maxBlock, 2);
        Big(out,
            18,
            (static_cast<uint64_t>(entry.rate) << 44) | (static_cast<uint64_t>(entry.channels - 1) << 41) |
                (static_cast<uint64_t>(first.depth - 1) << 36) | entry.frames,
            8);
        // MD5 and compressed frame-size bounds are unknown, as permitted by
        // STREAMINFO. All original frames and their CRCs are kept unchanged.
        out.insert(out.end(), data.begin(), data.end());
        return result;
    }
} // namespace tool::mw19::sound
