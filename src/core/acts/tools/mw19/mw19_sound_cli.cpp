#include <includes.hpp>
#include "mw19_sound.hpp"
#include <fstream>
#include <zlib.h>

namespace {
    int mw19soundtest(int argc, const char* argv[]) {
        using namespace tool::mw19;
        std::filesystem::path output = "output/mw19-sound-test.json";
        std::filesystem::path inputFile;
        schema::Json report{ { "test_only", true }, { "complete", false }, { "success", false } };
        auto writeReport = [&] {
            if (!inputFile.empty() &&
                (std::filesystem::weakly_canonical(output) == std::filesystem::weakly_canonical(inputFile) ||
                 (std::filesystem::exists(output) && std::filesystem::exists(inputFile) &&
                  std::filesystem::equivalent(output, inputFile))))
                throw std::runtime_error("Sound test report must not overwrite its input bank");
            schema::WriteJsonAtomic(output, report);
        };
        try {
            if (argc < 4)
                return tool::BAD_USAGE;
            const std::filesystem::path file = argv[2];
            inputFile = file;
            const std::string value = argv[3];
            size_t end{};
            const auto key = std::stoull(value, &end, value.starts_with("0x") || value.starts_with("0X") ? 16 : 10);
            if (value.empty() || value[0] == '-' || end != value.size() || key > UINT32_MAX)
                throw std::runtime_error("Expected a 32-bit sound key");
            for (int i = 4; i < argc; ++i) {
                if (std::string(argv[i]) != "-o" || ++i >= argc)
                    throw std::runtime_error("Expected -o report.json");
                output = argv[i];
            }
            report["archive"] = file.string();
            report["key"] = key;
            std::ifstream input(file, std::ios::binary);
            if (!input)
                throw std::runtime_error("Cannot open sound bank");
            const auto size = std::filesystem::file_size(file);
            if (size > INT64_MAX)
                throw std::runtime_error("Sound bank exceeds supported file range");
            uint64_t bytesRead{};
            schema::MemoryReader read = [&](uint64_t at, void* out, size_t n) {
                if (at > size || n > size - at)
                    return false;
                input.clear();
                input.seekg(static_cast<std::streamoff>(at));
                input.read(static_cast<char*>(out), static_cast<std::streamsize>(n));
                bytesRead += static_cast<uint64_t>(input.gcount());
                return input.good() && static_cast<size_t>(input.gcount()) == n;
            };
            sound::Bank bank(read, size);
            report["indexed_entries"] = bank.Entries().size();
            if (auto entry = bank.Find(static_cast<uint32_t>(key)))
                report["sample"] = sound::Describe(*entry);
            const auto flac = bank.ExtractFlac(static_cast<uint32_t>(key));
            report["payload_bytes"] = flac.data.size();
            report["payload_crc32"] = crc32(0, flac.data.data(), static_cast<uInt>(flac.data.size()));
            report["file_bytes_read"] = bytesRead;
            report["validation"] = "SAB bounds; FLAC frame headers, sequence, sample counts and CRCs";
            report["complete"] = true;
            report["success"] = true;
            writeReport();
            LOG_INFO(
                "Verified one sound: {} FLAC bytes, CRC32 {:08x}; report only: {}",
                flac.data.size(),
                report.at("payload_crc32").get<uint32_t>(),
                output.string()
            );
            return tool::OK;
        } catch (const schema::PayloadUnavailable& error) {
            report["status"] = "unavailable";
            report["reason"] = error.reason;
            report["error"] = error.what();
        } catch (const std::exception& error) {
            report["status"] = "failed";
            report["error"] = error.what();
        }
        report["success"] = false;
        report["complete"] = false;
        try {
            writeReport();
        } catch (...) {
            LOG_ERROR("Cannot persist sound test report");
        }
        LOG_ERROR("Sound capability test failed: {}", report.at("error").get<std::string>());
        return tool::BASIC_ERROR;
    }
    ADD_TOOL(
        mw19soundtest, "mw19", " <bank.sabs|bank.sabl> <key> [-o report.json]",
        "Test one Replay sound entry as FLAC in memory; write a report only", mw19soundtest
    );
} // namespace
