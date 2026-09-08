#include "tools/mw19/mw19_payload.hpp"
#include <algorithm>
#include <cstring>
#include <iostream>

using namespace tool::mw19::schema;
int main() {
    try {
        constexpr uint64_t base = 0x10000;
        std::vector<uint8_t> memory(1024);
        auto put = [&](size_t offset, auto value) { std::memcpy(memory.data() + offset, &value, sizeof(value)); };
        auto check = [](bool ok, const char* message) {
            if (!ok)
                throw std::runtime_error(message);
        };
        MemoryReader read = [&](uint64_t address, void* output, size_t size) {
            if (address < base || address - base > memory.size() || size > memory.size() - (address - base))
                return false;
            std::memcpy(output, memory.data() + address - base, size);
            return true;
        };
        const std::vector<uint8_t> expected{ 0, 1, 0xff, 0x80, 'M', 'E', 'S', 'H', 0 };
        std::copy(expected.begin(), expected.end(), memory.begin() + 256);
        for (const std::string pool : { "streamkey", "xmodelsurfs" }) {
            const bool model = pool == "xmodelsurfs";
            check(HasPayloadExporter(pool), "Stream buffer exporter not registered");
            auto setup = [&](bool streamed, bool gpu = false) {
                std::fill(memory.begin(), memory.begin() + 256, 0);
                put(model ? 16 : 8, uint64_t{ 0x1234 });
                if (model) {
                    put(48, base + 128);
                    put(56, uint16_t{ 1 });
                    put(128, streamed ? uint64_t{ 0xdeadbeef } : base + 256);
                    put(136, static_cast<uint32_t>(expected.size()));
                    put(140, uint32_t{ streamed ? 1u : 0u });
                } else {
                    put(40, streamed ? uint64_t{ 0xdeadbeef } : base + 256);
                    put(56, static_cast<uint32_t>(expected.size()));
                    put(61, static_cast<uint8_t>((streamed ? 0 : 2) | (gpu ? 1 : 0)));
                }
            };
            setup(false);
            check(ExportPayload(read, pool, base).data == expected, "Resident buffer changed bytes");
            if (!model) {
                setup(false, true);
                check(ExportPayload(read, pool, base).data == expected, "Resident GPU stream selection failed");
            }
            setup(true);
            size_t calls = 0;
            PackageReader packages = [&](uint64_t key, size_t size) {
                check(key == 0x1234 && size == expected.size(), "Incorrect package key or size");
                ++calls;
                return expected;
            };
            check(
                ExportPayload(read, pool, base, 1024, "replay-1.20", packages).data == expected && calls == 1,
                "Streamed buffer differs from resident bytes"
            );
            bool unavailable = false;
            try {
                ExportPayload(read, pool, base);
            } catch (const PayloadUnavailable& error) {
                unavailable = error.reason == "stream_package_required";
            }
            check(unavailable, "Missing package was not explicitly unavailable");
            put(model ? 16 : 8, uint64_t{});
            unavailable = false;
            try {
                ExportPayload(read, pool, base, 1024, "replay-1.20", packages);
            } catch (const PayloadUnavailable& error) {
                unavailable = error.reason == "stream_key_unavailable";
            }
            check(unavailable && calls == 1, "Missing key was not rejected before package reads");
            setup(false);
            put(model ? 128 : 40, uint64_t{});
            unavailable = false;
            try {
                ExportPayload(read, pool, base);
            } catch (const PayloadUnavailable& error) {
                unavailable = error.reason == "resident_buffer_unavailable";
            }
            check(unavailable, "Missing resident buffer was not explicitly unavailable");
            auto fails = [&](auto action, const char* message) {
                bool failed = false;
                try {
                    action();
                } catch (const std::exception&) {
                    failed = true;
                }
                check(failed, message);
            };
            setup(true);
            fails(
                [&] {
                    ExportPayload(read, pool, base, 1024, "replay-1.20", [](uint64_t, size_t n) {
                        return std::vector<uint8_t>(n - 1);
                    });
                },
                "Short package buffer accepted"
            );
            fails(
                [&] { ExportPayload(read, pool, base, model ? 116 : 68, "replay-1.20", packages); },
                "Header and payload read budget exceeded"
            );
            fails([&] { ExportPayload(read, pool, base, 1024, "game-test", packages); }, "Wrong profile accepted");
            setup(false);
            put(model ? 128 : 40, UINT64_MAX - 3);
            fails([&] { ExportPayload(read, pool, base); }, "Resident data address overflow accepted");
            if (model) {
                put(48, UINT64_MAX - 3);
                fails([&] { ExportPayload(read, pool, base); }, "Shared header address overflow accepted");
                put(48, uint64_t{});
                unavailable = false;
                try {
                    ExportPayload(read, pool, base);
                } catch (const PayloadUnavailable& error) {
                    unavailable = error.reason == "model_shared_unavailable";
                }
                check(unavailable, "Missing model shared header was not unavailable");
            }
            setup(true);
            put(model ? 136 : 56, uint32_t{});
            check(ExportPayload(read, pool, base).data.empty(), "Empty stream requires package input");
        }
        std::cout << "StreamKey/model shared buffers: exact resident/XPak bytes, GPU flag, missing data, corruption, "
                     "profile and budget checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
