#include "tools/mw19/mw19_image.hpp"
#include <algorithm>
#include <cstring>
#include <iostream>
#include <stdexcept>

using namespace tool::mw19::schema;
int main() {
    try {
        constexpr uint64_t base = 0x10000;
        std::vector<uint8_t> memory(16384);
        auto put = [&](size_t at, auto value) { std::memcpy(memory.data() + at, &value, sizeof(value)); };
        auto setup = [&](uint32_t format,
                         uint32_t flags,
                         uint16_t width,
                         uint16_t height,
                         uint16_t depth,
                         uint16_t elements,
                         uint8_t mips,
                         uint32_t size) {
            std::fill(memory.begin(), memory.end(), uint8_t{ 0xcc });
            std::fill(memory.begin(), memory.begin() + 232, uint8_t{});
            put(20, format);
            put(24, flags);
            put(28, size);
            put(36, width);
            put(38, height);
            put(40, depth);
            put(42, elements);
            put(48, mips);
            put(224, base + 2048);
        };
        MemoryReader read = [&](uint64_t at, void* out, size_t n) {
            if (at < base || at - base > memory.size() || n > memory.size() - (at - base))
                return false;
            std::memcpy(out, memory.data() + at - base, n);
            return true;
        };
        auto check = [](bool ok, const char* message) {
            if (!ok)
                throw std::runtime_error(message);
        };
        auto word = [](const Payload& p, size_t i) {
            uint32_t value{};
            std::memcpy(&value, p.data.data() + i * 4, 4);
            return value;
        };
        auto fails = [&](const char* message) {
            bool failed{};
            try {
                ExportReplayImage(read, base, 8192);
            } catch (const std::exception&) {
                failed = true;
            }
            check(failed, message);
        };
        // Array layout must change from mip-major + padding to slice-major.
        setup(6, 0x20000, 2, 1, 1, 2, 2, 64);
        std::memcpy(memory.data() + 2048, "AAAAAAAA", 8);
        std::memcpy(memory.data() + 2064, "BBBBBBBB", 8);
        std::memcpy(memory.data() + 2080, "CCCC", 4);
        std::memcpy(memory.data() + 2096, "DDDD", 4);
        auto image = ExportReplayImage(read, base, 8192);
        check(
            image.data.size() == 172 &&
                std::string(image.data.begin() + 148, image.data.end()) == "AAAAAAAACCCCBBBBBBBBDDDD",
            "DDS array ordering or padding removal"
        );
        check(
            word(image, 0) == 0x20534444 && word(image, 1) == 124 && word(image, 19) == 32 &&
                word(image, 21) == 0x30315844 && word(image, 32) == 28 && word(image, 33) == 3 &&
                word(image, 35) == 2 && word(image, 5) == 8 && word(image, 7) == 2,
            "DDS array header"
        );
        // Cubemap and cube-array cardinality is cubes, not individual faces.
        for (uint16_t cubes : { uint16_t{ 1 }, uint16_t{ 2 } }) {
            setup(6, cubes == 1 ? 0x8000 : 0x28000, 1, 1, 1, cubes, 1, 16 * 6 * cubes);
            for (uint32_t face = 0; face < 6 * cubes; ++face)
                put(2048 + 16 * face, 0x11110000u + face);
            image = ExportReplayImage(read, base, 8192);
            check(
                image.data.size() == 148 + 24 * cubes && word(image, 28) == 0xfe00 && word(image, 34) == 4 &&
                    word(image, 35) == cubes && word(image, 37 + 6 * cubes - 1) == 0x11110000u + 6 * cubes - 1,
                "DDS cube cardinality or face data"
            );
        }
        // BC1 rounds partial 4x4 blocks up at each mip, then removes padding.
        setup(33, 0, 5, 3, 1, 1, 3, 48);
        std::fill(memory.begin() + 2048, memory.begin() + 2064, 'A');
        std::fill(memory.begin() + 2064, memory.begin() + 2072, 'B');
        std::fill(memory.begin() + 2080, memory.begin() + 2088, 'C');
        image = ExportReplayImage(read, base, 8192);
        check(
            word(image, 32) == 71 && word(image, 5) == 16 && image.data.size() == 180 &&
                std::string(image.data.begin() + 148, image.data.end()) ==
                    std::string(16, 'A') + std::string(8, 'B') + std::string(8, 'C'),
            "BC block rounding or mip padding"
        );
        // All declared native formats have a DDS mapping and consistent size.
        const uint32_t bytes[] = { 0, 1, 1,  1, 2, 2, 4, 4, 1, 2, 2,  4, 8, 2, 2, 4, 8,
                                   4, 8, 16, 2, 4, 8, 1, 2, 4, 8, 16, 4, 2, 4, 4, 4 };
        for (uint32_t format = 1; format <= 46; ++format) {
            const uint32_t size = format < 33                                    ? bytes[format]
                                  : format == 46                                 ? 4
                                  : format == 33 || format == 34 || format == 39 ? 8
                                                                                 : 16;
            setup(format, 0, 1, 1, 1, 1, 1, 16);
            image = ExportReplayImage(read, base, 8192);
            check(image.data.size() == 148 + size && word(image, 32) != 0, "Native pixel format size");
        }
        setup(1, 0x10000, 2, 2, 2, 1, 2, 32);
        image = ExportReplayImage(read, base, 8192);
        check(
            word(image, 6) == 2 && word(image, 33) == 4 && word(image, 28) == 0x200000 && image.data.size() == 157,
            "Volume DDS depth and mip sizes"
        );
        setup(1, 0x18000, 4, 1, 1, 1, 3, 48);
        image = ExportReplayImage(read, base, 8192);
        check(word(image, 33) == 2 && image.data.size() == 155, "1D DDS mip sizes");
        put(38, uint16_t{ 2 });
        fails("Invalid 1D height accepted");
        setup(6, 0, 2, 2, 1, 1, 1, 15);
        fails("Short pixel allocation accepted");
        put(28, uint32_t{ 16 });
        put(48, uint8_t{ 3 });
        fails("Excess image mips accepted");
        put(48, uint8_t{ 1 });
        put(20, uint32_t{ 48 });
        fails("Source-only pixel format accepted");
        put(20, uint32_t{ 6 });
        put(224, UINT64_MAX - 4);
        fails("Image address overflow accepted");
        put(224, base + 2048);
        bool bounded{};
        try {
            ExportReplayImage(read, base, 240);
        } catch (const std::exception&) {
            bounded = true;
        }
        check(bounded, "Combined image read budget exceeded");
        for (uint32_t flags : { 0u, 0x40u }) {
            put(24, flags);
            put(224, flags ? uint64_t{ 0xdeadbeef } : uint64_t{ 0 });
            bool unavailable{};
            try {
                ExportReplayImage(read, base, 8192);
            } catch (const PayloadUnavailable& error) {
                unavailable = error.reason == (flags ? "streamed_image" : "image_uploaded");
            }
            check(unavailable, "Absent/streamed image data not classified");
        }
        // Three XPak parts grow from the mip tail toward full resolution.
        setup(6, 0, 8, 8, 1, 1, 4, 352);
        for (size_t i = 0; i < 352; ++i)
            memory[2048 + i] = static_cast<uint8_t>(i);
        const auto resident = ExportReplayImage(read, base, 8192);
        put(24, uint32_t{ 0x40 });
        put(224, uint64_t{ 0xdeadbeef }); // Stream handle is never read as CPU memory.
        put(49, uint8_t{ 0xfe });         // Source offset deliberately differs from Replay.
        put(50, uint8_t{ 3 });
        const uint32_t sizes[]{ 32, 96, 352 }, mipCounts[]{ 2, 3, 4 };
        for (size_t part = 0; part < 3; ++part) {
            const auto at = 56 + 40 * part;
            put(at, uint64_t{ 100 + part });
            put(at + 32, (sizes[part] << 4) | mipCounts[part]);
            put(at + 36, uint16_t{ static_cast<uint16_t>(2 << part) });
            put(at + 38, uint16_t{ static_cast<uint16_t>(2 << part) });
        }
        size_t calls = 0;
        PackageReader packages = [&](uint64_t key, size_t count) {
            check(key >= 100 && key <= 102, "Unexpected XPak key");
            const auto part = key - 100;
            check(count == sizes[part] - (part ? sizes[part - 1] : 0), "Incorrect exclusive part size");
            ++calls;
            auto first = memory.begin() + 2048 + 352 - sizes[part];
            return std::vector<uint8_t>(first, first + count);
        };
        check(
            ExportReplayImage(read, base, 8192, packages).data == resident.data && calls == 3,
            "Streamed DDS differs from resident mip chain"
        );
        auto badStream = [&](const char* message) {
            bool failed = false;
            try {
                ExportReplayImage(read, base, 8192, packages);
            } catch (const std::exception&) {
                failed = true;
            }
            check(failed, message);
        };
        put(50, uint8_t{ 5 });
        badStream("More than four streamed parts accepted");
        put(50, uint8_t{ 2 });
        badStream("Incomplete streamed mip chain accepted");
        put(50, uint8_t{ 3 });
        put(88, uint32_t{ (31 << 4) | 2 });
        badStream("Incorrect cumulative part size accepted");
        put(88, uint32_t{ (32 << 4) | 2 });
        put(92, uint16_t{ 4 });
        badStream("Incorrect streamed mip dimensions accepted");
        put(92, uint16_t{ 2 });
        check(calls == 3, "Malformed stream header read a package before validation finished");
        bool missing = false;
        try {
            ExportReplayImage(read, base, 8192, [](uint64_t, size_t) -> std::vector<uint8_t> {
                throw PayloadUnavailable("package_key_missing", "Fixture missing key");
            });
        } catch (const PayloadUnavailable& error) {
            missing = error.reason == "package_key_missing";
        }
        check(missing, "Missing stream entry was not preserved as unavailable");
        bool shortPart = false;
        try {
            ExportReplayImage(read, base, 8192, [](uint64_t, size_t n) { return std::vector<uint8_t>(n - 1); });
        } catch (const std::runtime_error&) {
            shortPart = true;
        }
        check(shortPart, "Short decoded stream part accepted");
        std::cout << "Replay DDS: 46 formats, mip/array ordering, cube arrays, volume/1D, BC rounding, bounds and "
                     "unavailable data, native stream count and XPak mip assembly passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
