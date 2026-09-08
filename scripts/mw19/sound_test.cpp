#include "tools/mw19/mw19_sound.hpp"
#include <cstring>
#include <fstream>
#include <iostream>
#include <io.h>
#include <fcntl.h>

using namespace tool::mw19;
using schema::MemoryReader;
using schema::PayloadUnavailable;
namespace {
    uint32_t Crc(const std::vector<uint8_t>& bytes, unsigned bits, uint32_t polynomial) {
        uint32_t crc = 0;
        for (auto b : bytes) {
            crc ^= uint32_t(b) << (bits - 8);
            for (int bit = 0; bit < 8; ++bit)
                crc = (crc << 1) ^ ((crc & (1u << (bits - 1))) ? polynomial : 0);
        }
        return crc & ((1u << bits) - 1);
    }
    std::vector<uint8_t> Frame(uint8_t number, uint8_t samples, bool variable = false) {
        std::vector<uint8_t> b{ 0xff,   static_cast<uint8_t>(variable ? 0xf9 : 0xf8),
                                0x69,   0x08,
                                number, static_cast<uint8_t>(samples - 1) };
        b.push_back(static_cast<uint8_t>(Crc(b, 8, 7)));
        b.insert(b.end(), { 0, 0x12, 0x34 }); // Constant mono subframe, signed 16-bit 0x1234.
        const auto crc = Crc(b, 16, 0x8005);
        b.push_back(static_cast<uint8_t>(crc >> 8));
        b.push_back(static_cast<uint8_t>(crc));
        return b;
    }
    template<typename T>
    void Put(std::vector<uint8_t>& b, size_t at, T value) {
        std::memcpy(b.data() + at, &value, sizeof(value));
    }
    void Emit(const schema::Payload& p) {
        _setmode(_fileno(stdout), _O_BINARY);
        std::cout.write(reinterpret_cast<const char*>(p.data.data()), p.data.size());
    }
} // namespace
int main(int argc, char** argv) {
    try {
        if (argc == 4 && std::string(argv[1]) == "--extract") {
            std::ifstream file(argv[2], std::ios::binary | std::ios::ate);
            if (!file)
                throw std::runtime_error("Cannot open bank");
            auto size = static_cast<uint64_t>(file.tellg());
            MemoryReader r = [&](uint64_t at, void* out, size_t n) {
                file.clear();
                file.seekg(static_cast<std::streamoff>(at));
                file.read(static_cast<char*>(out), n);
                return file.good() && static_cast<size_t>(file.gcount()) == n;
            };
            sound::Bank bank(r, size);
            Emit(bank.ExtractFlac(static_cast<uint32_t>(std::stoul(argv[3], nullptr, 0))));
            return 0;
        }
        auto check = [](bool ok, const char* message) {
            if (!ok)
                throw std::runtime_error(message);
        };
        auto fails = [&](auto f, const char* message) {
            bool failed = false;
            try {
                f();
            } catch (const std::exception&) {
                failed = true;
            }
            check(failed, message);
        };
        std::vector<uint8_t> bank;
        auto setup = [&] {
            bank.assign(1024, 0);
            Put(bank, 0, 0x23585532u);
            Put(bank, 4, 10u);
            Put(bank, 8, 44u);
            Put(bank, 12, 16u);
            Put(bank, 16, 64u);
            Put(bank, 20, 1u);
            Put(bank, 28, 16u);
            Put(bank, 32, uint64_t{ 1024 });
            Put(bank, 40, uint64_t{ 688 });
            Put(bank, 48, uint64_t{ 752 });
            const auto a = Frame(0, 16), b = Frame(1, 5);
            Put(bank, 688, 0x1234u);
            Put(bank, 692, static_cast<uint32_t>(a.size() + b.size()));
            Put(bank, 696, 4u);
            Put(bank, 700, 21u);
            Put(bank, 708, uint64_t{ 800 });
            Put(bank, 716, 44100u);
            bank[720] = 1;
            bank[721] = 1;
            bank[722] = 8;
            std::copy(a.begin(), a.end(), bank.begin() + 804);
            std::copy(b.begin(), b.end(), bank.begin() + 804 + a.size());
        };
        size_t bytesRead = 0;
        MemoryReader read = [&](uint64_t at, void* out, size_t n) {
            if (at > bank.size() || n > bank.size() - at)
                return false;
            bytesRead += n;
            std::memcpy(out, bank.data() + at, n);
            return true;
        };
        setup();
        sound::Bank parsed(read, bank.size());
        check(bytesRead == 688 + 44 && parsed.Entries().size() == 1, "Bank constructor read audio or missed index");
        const auto flac = parsed.ExtractFlac(0x1234);
        check(bytesRead == 688 + 44 + 24 && flac.data.size() == 66, "Sample byte range incorrect");
        check(std::memcmp(flac.data.data(), "fLaC\x80\0\0\x22", 8) == 0, "FLAC metadata envelope invalid");
        check(
            flac.data[8] == 0 && flac.data[9] == 16 && flac.data[10] == 0 && flac.data[11] == 16,
            "FLAC block bounds invalid"
        );
        check(std::equal(flac.data.begin() + 42, flac.data.end(), bank.begin() + 804), "Original FLAC frames changed");
        if (argc > 1) {
            Emit(flac);
            return 0;
        }
        check(parsed.Find(0x1234) && !parsed.Find(0x1235), "Sound key lookup failed");
        check(sound::Describe(*parsed.Find(0x1234)).at("looping") == true, "Loop metadata lost");
        bool missing = false;
        try {
            parsed.ExtractFlac(0x1235);
        } catch (const PayloadUnavailable& e) {
            missing = e.reason == "sound_key_missing";
        }
        check(missing, "Missing sound key not reported unavailable");
        auto variable = Frame(0, 16, true);
        auto last = Frame(16, 5, true);
        variable.insert(variable.end(), last.begin(), last.end());
        auto entry = *parsed.Find(0x1234);
        check(sound::RestoreFlac(variable, entry, 1024).data.size() == 66, "Variable FLAC frame sequence failed");
        auto shortFrame = Frame(0, 1);
        entry.size = static_cast<uint32_t>(shortFrame.size());
        entry.frames = 1;
        check(sound::RestoreFlac(shortFrame, entry, 1024).data[11] == 16, "Single short final frame metadata invalid");
        setup();
        bank[820] ^= 1;
        fails(
            [&] {
                sound::Bank b(read, bank.size());
                b.ExtractFlac(0x1234);
            },
            "Corrupt FLAC frame accepted"
        );
        setup();
        bank[810] ^= 1;
        fails(
            [&] {
                sound::Bank b(read, bank.size());
                b.ExtractFlac(0x1234);
            },
            "Corrupt FLAC header accepted"
        );
        setup();
        Put(bank, 700, 22u);
        fails(
            [&] {
                sound::Bank b(read, bank.size());
                b.ExtractFlac(0x1234);
            },
            "Incorrect sample count accepted"
        );
        setup();
        Put(bank, 716, 48000u);
        fails(
            [&] {
                sound::Bank b(read, bank.size());
                b.ExtractFlac(0x1234);
            },
            "Incorrect rate accepted"
        );
        setup();
        auto skipped = Frame(2, 5);
        std::copy(skipped.begin(), skipped.end(), bank.begin() + 816);
        fails(
            [&] {
                sound::Bank b(read, bank.size());
                b.ExtractFlac(0x1234);
            },
            "Discontinuous FLAC frame sequence accepted"
        );
        setup();
        Put(bank, 692, 23u);
        fails(
            [&] {
                sound::Bank b(read, bank.size());
                b.ExtractFlac(0x1234);
            },
            "Truncated sample accepted"
        );
        for (auto offset : { 0, 4, 8, 12, 28 }) {
            setup();
            bank[offset] ^= 1;
            fails([&] { sound::Bank b(read, bank.size()); }, "Unsupported SAB layout accepted");
        }
        setup();
        Put(bank, 40, uint64_t{ UINT64_MAX - 4 });
        fails([&] { sound::Bank b(read, bank.size()); }, "Index address overflow accepted");
        setup();
        Put(bank, 48, uint64_t{ 700 });
        fails([&] { sound::Bank b(read, bank.size()); }, "Overlapping checksum table accepted");
        setup();
        Put(bank, 708, uint64_t{ 700 });
        fails([&] { sound::Bank b(read, bank.size()); }, "Sample overlapping metadata accepted");
        setup();
        Put(bank, 708, uint64_t{ 1010 });
        fails([&] { sound::Bank b(read, bank.size()); }, "Sample extent exceeds file accepted");
        setup();
        Put(bank, 20, 2u);
        Put(bank, 48, uint64_t{ 900 });
        std::copy(bank.begin() + 688, bank.begin() + 732, bank.begin() + 732);
        fails([&] { sound::Bank b(read, bank.size()); }, "Duplicate key accepted");
        Put(bank, 732, 0x1200u);
        auto originalBank = bank;
        sound::Bank storageOrder(read, bank.size());
        if (!storageOrder.Find(0x1200) || !storageOrder.Find(0x1234) || bank != originalBank)
            throw std::runtime_error("Storage-order SAB lookup or byte preservation failed");
        setup();
        fails([&] { sound::Bank b(read, bank.size(), 700); }, "Metadata budget ignored");
        fails(
            [&] {
                sound::Bank b(read, bank.size());
                b.ExtractFlac(0x1234, 750);
            },
            "Sound output budget ignored"
        );
        for (bool hybrid : { false, true }) {
            setup();
            if (hybrid)
                Put(bank, 704, 4u);
            else
                bank[722] = 9;
            sound::Bank b(read, bank.size());
            auto before = bytesRead;
            bool unavailable = false;
            try {
                b.ExtractFlac(0x1234);
            } catch (const PayloadUnavailable& e) {
                unavailable = e.reason == "sound_codec_unavailable";
            }
            check(unavailable && bytesRead == before, "Unsupported codec read bytes or claimed success");
        }
        // Both sound pools share the validated StreamKey-backed SAB container.
        setup();
        std::vector<uint8_t> memory(2048, 0);
        constexpr uint64_t base = 0x10000;
        Put(memory, 496, base + 512);
        Put(memory, 552, base + 1024);
        Put(memory, 568, uint32_t{ 1024 });
        memory[573] = 2;
        std::copy(bank.begin(), bank.end(), memory.begin() + 1024);
        MemoryReader resident = [&](uint64_t at, void* out, size_t n) {
            if (at < base || at - base > memory.size() || n > memory.size() - (at - base))
                return false;
            std::memcpy(out, memory.data() + at - base, n);
            return true;
        };
        for (const std::string pool : { "soundbank", "soundbanktransient" }) {
            check(schema::HasPayloadExporter(pool), "Sound pool exporter missing");
            check(schema::ExportPayload(resident, pool, base).data == bank, "SAB container bytes changed");
            memory[573] = 0;
            Put(memory, 520, uint64_t{ 0x8765 });
            size_t calls = 0;
            auto p = schema::ExportPayload(resident, pool, base, 4096, "replay-1.20", [&](uint64_t key, size_t n) {
                check(key == 0x8765 && n == bank.size(), "Incorrect sound package request");
                ++calls;
                return bank;
            });
            check(calls == 1 && p.data == bank && p.extension == ".sabl", "XPak sound container failed");
            bool unavailable = false;
            try {
                schema::ExportPayload(resident, pool, base);
            } catch (const PayloadUnavailable&) {
                unavailable = true;
            }
            check(unavailable, "Nonresident sound without package was not unavailable");
            memory[573] = 2;
            Put(memory, 568, uint32_t{});
            unavailable = false;
            try {
                schema::ExportPayload(resident, pool, base);
            } catch (const PayloadUnavailable& error) {
                unavailable = error.reason == "sound_bank_empty";
            }
            check(unavailable, "Empty loaded bank was not reported unavailable");
            Put(memory, 568, uint32_t{ 1024 });
            fails([&] { schema::ExportPayload(resident, pool, base, 1000); }, "Sound bank combined budget ignored");
            fails(
                [&] { schema::ExportPayload(resident, pool, base, 4096, "game-test"); },
                "Replay bank layout applied to game-test"
            );
        }
        std::cout << "Replay sound tests passed: SAB index/bounds, resident/XPak banks, FLAC "
                     "headers/CRCs/sequences/counts, unavailable codecs and budgets. No asset files written.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
