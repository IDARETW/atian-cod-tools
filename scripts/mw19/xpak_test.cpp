#include "tools/mw19/mw19_xpak.hpp"
#include <lz4.h>
#include <algorithm>
#include <cstring>
#include <iostream>
#include <stdexcept>

using namespace tool::mw19::xpak;
namespace {
    void Check(bool ok, const char* message) {
        if (!ok)
            throw std::runtime_error(message);
    }
    template<class F>
    void Fails(F action, const char* message) {
        bool failed{};
        try {
            action();
        } catch (const std::exception&) {
            failed = true;
        }
        Check(failed, message);
    }
    template<class T>
    void Put(std::vector<uint8_t>& bytes, size_t at, T value) {
        std::memcpy(bytes.data() + at, &value, sizeof(value));
    }
    std::vector<uint8_t> Block(std::span<const uint8_t> data, uint32_t codec = 0, uint32_t destination = 0) {
        std::vector<uint8_t> block((128 + data.size() + 127) & ~size_t{ 127 });
        Put(block, 0, uint32_t{ 1 });
        Put(block, 4, destination);
        Put(block, 8, (codec << 24) | static_cast<uint32_t>(data.size()));
        std::copy(data.begin(), data.end(), block.begin() + 128);
        return block;
    }
    ReadAt Reader(const std::vector<uint8_t>& bytes) {
        return [&bytes](uint64_t at, void* target, size_t length) {
            if (at > bytes.size() || length > bytes.size() - at)
                return false;
            std::memcpy(target, bytes.data() + at, length);
            return true;
        };
    }
} // namespace
int main() {
    try {
        const std::vector<uint8_t> text{ 'A', 'B', 'C', 'D' };
        auto raw = Block(text);
        Check(DecodeBlocks(raw, 4) == text, "Raw XPak block");
        auto tail = Block(std::span(text).subspan(2), 0, 2), front = Block(std::span(text).first(2));
        tail.insert(tail.end(), front.begin(), front.end());
        Check(DecodeBlocks(tail, 4) == text, "Reordered XPak groups");
        auto overlap = raw;
        overlap.insert(overlap.end(), raw.begin(), raw.end());
        Fails([&] { DecodeBlocks(overlap, 4); }, "Overlapping writes accepted");
        Fails([&] { DecodeBlocks(raw, 5); }, "Output gap accepted");
        Fails([&] { DecodeBlocks(raw, 3); }, "Output overflow accepted");
        Fails([&] { DecodeBlocks(raw, 4, {}, 3); }, "Budget ignored");
        auto bad = raw;
        Put(bad, 0, uint32_t{ 31 });
        Fails([&] { DecodeBlocks(bad, 4); }, "Invalid block count accepted");
        bad = raw;
        bad.resize(130);
        Fails([&] { DecodeBlocks(bad, 4); }, "Short compressed block accepted");
        for (auto codec : { 1u, 2u, 3u, 8u, 255u }) {
            bad = Block(text, codec);
            Fails([&] { DecodeBlocks(bad, 4); }, "Unsupported Replay codec accepted");
        }
        std::vector<uint8_t> repeated(4096, 0x37), compressed(LZ4_compressBound(4096));
        auto length = LZ4_compress_default(
            reinterpret_cast<const char*>(repeated.data()),
            reinterpret_cast<char*>(compressed.data()),
            4096,
            static_cast<int>(compressed.size())
        );
        Check(length > 0, "LZ4 fixture compression");
        compressed.resize(length);
        for (auto codec : { 4u, 5u })
            Check(DecodeBlocks(Block(compressed, codec), repeated.size()) == repeated, "LZ4 XPak decode");
        for (auto codec : { 6u, 7u }) {
            size_t calls{};
            auto decoded =
                DecodeBlocks(Block(text, codec), 17, [&](std::span<const uint8_t> in, std::span<uint8_t> out) {
                    ++calls;
                    Check(in.size() == 4 && out.size() == 17, "Oodle callback extents");
                    std::fill(out.begin(), out.end(), 0x77);
                    return true;
                });
            Check(calls == 1 && decoded == std::vector<uint8_t>(17, 0x77), "Oodle dispatch");
            Fails([&] { DecodeBlocks(Block(text, codec), 17); }, "Missing Oodle accepted");
        }
        // Native chunks decode at most 0x3ffe0 bytes, independent of stored size.
        auto first = Block(text, 6), last = Block(text, 6, 0x3ffe0);
        first.insert(first.end(), last.begin(), last.end());
        size_t calls{};
        auto large = DecodeBlocks(first, 0x3ffe0 + 5, [&](auto, std::span<uint8_t> out) {
            Check(out.size() == (calls++ ? 5 : 0x3ffe0), "Native maximum decoded chunk");
            std::fill(out.begin(), out.end(), uint8_t(calls));
            return true;
        });
        Check(large.front() == 1 && large.back() == 2, "Multi-group output");
        // Archive index and metadata framing, entirely synthetic in memory.
        std::vector<uint8_t> file(2048);
        Put(file, 0, uint32_t{ 0x4950414b });
        Put(file, 6, uint16_t{ 13 });
        Put(file, 16, uint64_t{ file.size() });
        Put(file, 0x138, uint64_t{ 1 });
        Put(file, 0x140, uint64_t{ 1024 });
        Put(file, 0x148, uint64_t{ 256 });
        Put(file, 0x150, uint64_t{ 1 });
        Put(file, 0x158, uint64_t{ 1280 });
        Put(file, 0x160, uint64_t{ 24 });
        Put(file, 1280, uint64_t{ 123 });
        Put(file, 1288, uint64_t{});
        Put(file, 1296, uint64_t{ 0x8000000000000100 });
        std::copy(raw.begin(), raw.end(), file.begin() + 1024);
        const std::string meta = "size0: 4\noffset0: 0";
        Put(file, 0x180, uint64_t{ 1 });
        Put(file, 0x188, uint64_t{ 1408 });
        Put(file, 0x190, uint64_t{ 16 + meta.size() });
        Put(file, 1408, uint64_t{ 123 });
        Put(file, 1416, uint64_t{ meta.size() });
        std::copy(meta.begin(), meta.end(), file.begin() + 1424);
        Archive archive(Reader(file), file.size());
        Check(
            archive.Entries().size() == 1 && archive.Find(123)->offset == 1024 && !archive.Find(124),
            "XPak index lookup"
        );
        Check(archive.Metadata(123) == meta && archive.Extract(123, 4) == text, "XPak metadata/extraction");
        Fails([&] { archive.Extract(124, 4); }, "Unknown key accepted");
        auto corrupt = file;
        Put(corrupt, 1296, uint64_t{ 0x8000000000000101 });
        Fails([&] { Archive invalid(Reader(corrupt), corrupt.size()); }, "Entry exceeds data section");
        corrupt = file;
        Put(corrupt, 6, uint16_t{ 11 });
        Fails([&] { Archive invalid(Reader(corrupt), corrupt.size()); }, "Foreign XPak version accepted");
        corrupt = file;
        Put(corrupt, 0x160, uint64_t{ UINT64_MAX });
        Fails([&] { Archive invalid(Reader(corrupt), corrupt.size()); }, "Oversized XPak index accepted");
        Put(file, 1416, uint64_t{ UINT64_MAX });
        Fails([&] { archive.Metadata(123); }, "Oversized metadata accepted");
        std::cout << "Replay XPak: indexes, metadata, raw/LZ4/Oodle dispatch, chunk layout, coverage and corruption "
                     "bounds passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
