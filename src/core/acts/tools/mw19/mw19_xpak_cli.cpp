#include <includes.hpp>
#include "mw19_xpak.hpp"
#include <deps/oodle.hpp>
#include <zlib.h>

namespace {
    uint64_t Integer(const std::string& text) {
        if (text.empty() || text[0] == '-')
            throw std::runtime_error("Expected a nonnegative integer");
        size_t end{};
        auto value = std::stoull(text, &end, text.starts_with("0x") || text.starts_with("0X") ? 16 : 10);
        if (end != text.size())
            throw std::runtime_error("Invalid integer: " + text);
        return value;
    }
    int mw19xpaktest(int argc, const char* argv[]) {
        using namespace tool::mw19;
        using schema::Json;
        std::filesystem::path output = "output/mw19-xpak-test.json";
        Json report{ { "test_only", true }, { "complete", false }, { "success", false } };
        try {
            if (argc < 4)
                return tool::BAD_USAGE;
            const std::filesystem::path file = argv[2];
            const uint64_t key = Integer(argv[3]);
            std::optional<uint64_t> expected;
            std::filesystem::path oodlePath;
            for (int i = 4; i < argc; ++i) {
                const std::string option = argv[i];
                if (i + 1 >= argc)
                    throw std::runtime_error("Missing option value");
                if (option == "--size")
                    expected = Integer(argv[++i]);
                else if (option == "--oodle")
                    oodlePath = argv[++i];
                else if (option == "-o")
                    output = argv[++i];
                else
                    throw std::runtime_error("Unknown XPak test option: " + option);
            }
            report["archive"] = file.string();
            report["key"] = key;
            auto archive = xpak::Archive::Open(file);
            report["indexed_entries"] = archive.Entries().size();
            const auto entry = archive.Find(key);
            if (!entry)
                throw std::runtime_error("XPak key was not found");
            report["compressed"] = entry->compressed;
            report["stored_bytes"] = entry->size;
            if (auto metadata = archive.Metadata(key)) {
                Json fields = Json::object();
                std::istringstream lines(*metadata);
                std::string line;
                while (std::getline(lines, line)) {
                    auto colon = line.find(": ");
                    if (colon == std::string::npos)
                        continue;
                    const auto name = line.substr(0, colon);
                    if (fields.contains(name))
                        throw std::runtime_error("Duplicate XPak metadata field");
                    fields[name] = line.substr(colon + 2);
                }
                report["metadata"] = fields;
                if (!expected && fields.contains("size0") && fields.value("offset0", std::string{}) == "0" &&
                    !fields.contains("size1")) {
                    expected = Integer(fields.at("size0").get<std::string>());
                    report["size_source"] = "xpak_metadata_size0";
                }
            }
            if (!expected)
                throw std::runtime_error("Supply --size; archive metadata has no single complete payload size");
            if (*expected > 128 * 1024 * 1024)
                throw std::runtime_error("XPak test exceeds 128 MiB byte budget");
            deps::oodle::Oodle oodle;
            xpak::OodleDecoder decode;
            if (!oodlePath.empty()) {
                if (!oodlePath.is_absolute())
                    throw std::runtime_error("Use an absolute Oodle DLL path");
                if (!oodle.LoadOodle(oodlePath.string().c_str()))
                    throw std::runtime_error("Cannot load supplied Oodle library");
                decode = [&](std::span<const uint8_t> input, std::span<uint8_t> out) {
                    const auto size = oodle.Decompress(
                        input.data(),
                        static_cast<uint32_t>(input.size()),
                        out.data(),
                        static_cast<uint32_t>(out.size()),
                        deps::oodle::OODLE_FS_YES
                    );
                    return size >= 0 && static_cast<size_t>(size) == out.size();
                };
            }
            const auto bytes = archive.Extract(key, static_cast<size_t>(*expected), decode);
            report["payload_bytes"] = bytes.size();
            report["payload_crc32"] = crc32(0, bytes.data(), static_cast<uInt>(bytes.size()));
            report["complete"] = true;
            report["success"] = true;
            schema::WriteJsonAtomic(output, report);
            LOG_INFO(
                "Verified one XPak entry: {} stored bytes -> {} payload bytes; CRC32 {:08x}. Report only: {}",
                entry->size,
                bytes.size(),
                report.at("payload_crc32").get<uint32_t>(),
                output.string()
            );
            return tool::OK;
        } catch (const std::exception& error) {
            report["error"] = error.what();
            try {
                schema::WriteJsonAtomic(output, report);
            } catch (...) {
                LOG_ERROR("Cannot persist XPak test report");
            }
            LOG_ERROR("XPak test failed: {}", error.what());
            return tool::BASIC_ERROR;
        }
    }
    ADD_TOOL(
        mw19xpaktest, "mw19", " <archive.xpak> <key> [--size bytes] [--oodle absolute.dll] [-o report.json]",
        "Test one Replay XPak payload in memory and write a report only", mw19xpaktest
    );
} // namespace
