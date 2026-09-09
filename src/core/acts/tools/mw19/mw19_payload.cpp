#ifndef MW19_SCHEMA_STANDALONE
#include <includes.hpp>
#endif
#include "mw19_payload.hpp"
#include "mw19_image.hpp"
#include "mw19_sound.hpp"
#include <algorithm>
#include <cstring>
#include <sstream>
#include <streambuf>
#include <stdexcept>
#include <zlib.h>

namespace tool::mw19::schema {
    namespace {
        class BoundedJsonBuffer final : public std::streambuf {
            size_t limit;

          public:
            std::vector<uint8_t> data;
            bool exceeded{};
            explicit BoundedJsonBuffer(size_t maxBytes) : limit(maxBytes) {
                data.reserve(std::min(maxBytes, size_t{ 65536 }));
            }
            std::streamsize xsputn(const char* source, std::streamsize count) override {
                if (count <= 0)
                    return 0;
                const auto length = static_cast<size_t>(count);
                if (length > limit - data.size()) {
                    exceeded = true;
                    return 0;
                }
                data.insert(data.end(), source, source + length);
                return count;
            }
            int_type overflow(int_type value) override {
                if (traits_type::eq_int_type(value, traits_type::eof()))
                    return traits_type::not_eof(value);
                if (data.size() == limit) {
                    exceeded = true;
                    return traits_type::eof();
                }
                data.push_back(static_cast<uint8_t>(traits_type::to_char_type(value)));
                return value;
            }
        };
        template<typename T>
        T Value(const std::vector<uint8_t>& bytes, size_t offset) {
            if (offset > bytes.size() || sizeof(T) > bytes.size() - offset)
                throw std::runtime_error("Truncated payload header");
            T value{};
            std::memcpy(&value, bytes.data() + offset, sizeof(value));
            return value;
        }
        class Reader {
            const MemoryReader& read;
            size_t remaining;

          public:
            Reader(const MemoryReader& r, size_t maxBytes) : read(r), remaining(maxBytes) {}
            std::vector<uint8_t> Bytes(uint64_t address, size_t length) {
                if (length > remaining || length > UINT64_MAX - address)
                    throw std::runtime_error("Payload exceeds read budget");
                remaining -= length;
                std::vector<uint8_t> result(length);
                if (length && (!address || !read(address, result.data(), length)))
                    throw std::runtime_error("Cannot read payload");
                return result;
            }
            template<typename T>
            T Number(uint64_t address) {
                auto bytes = Bytes(address, sizeof(T));
                T value{};
                std::memcpy(&value, bytes.data(), sizeof(T));
                return value;
            }
            std::string String(uint64_t address) {
                MemoryReader bounded = [&](uint64_t a, void* b, size_t n) {
                    auto v = Bytes(a, n);
                    std::memcpy(b, v.data(), n);
                    return true;
                };
                return ReadString(bounded, address);
            }
            size_t Count(int64_t value) {
                if (value < 0 || static_cast<uint64_t>(value) > remaining)
                    throw std::runtime_error("Invalid payload count");
                return static_cast<size_t>(value);
            }
        };
        Payload Text(std::string text, std::string extension, std::string format) {
            return { std::move(extension), { text.begin(), text.end() }, std::move(format) };
        }
        std::string Csv(const std::string& value) {
            std::string out = "\"";
            for (char c : value) {
                if (c == '"')
                    out += '"';
                out += c;
            }
            return out + '"';
        }
    } // namespace

    Payload ExportStructuredAsset(const Json& profile, const Json& pool, Json inspection, size_t maxBytes) {
        if (!inspection.is_object() || inspection.at("type") != pool.at("root_type") || !inspection.contains("fields"))
            throw std::runtime_error("Structured asset does not match pool root");
        if (inspection.at("read_errors").get<size_t>())
            throw std::runtime_error("Structured asset contains unreadable fields");
        for (const char* counter : { "unresolved_pointers", "unresolved_unions", "external_payloads" })
            if (inspection.at(counter).get<size_t>())
                throw PayloadUnavailable("structured_data_incomplete", "Structured asset contains unresolved data");
        Json document{ { "format", "mw19-asset-json" }, { "format_version", 1 },
                       { "profile", profile.at("id") }, { "pool", pool.at("name") },
                       { "pool_id", pool.at("id") },    { "root_type", pool.at("root_type") } };
        document["asset"] = std::move(inspection);
        BoundedJsonBuffer buffer(maxBytes);
        std::ostream output(&buffer);
        output.exceptions(std::ios::failbit | std::ios::badbit);
        try {
            output << document << '\n';
        } catch (...) {
            if (buffer.exceeded)
                throw std::runtime_error("Structured asset exceeds output budget");
            throw;
        }
        return { ".asset.json", std::move(buffer.data), "mw19-asset-json-v1" };
    }

    bool HasPayloadExporter(const std::string& pool) {
        static const std::set<std::string> supported = {
            "rawfile",         "scriptfile",  "luafile",       "stringtable",       "localize",
            "netconststrings", "ttf",         "soundbanklist", "computeshader",     "libshader",
            "vertexshader",    "hullshader",  "domainshader",  "pixelshader",       "image",
            "streamkey",       "xmodelsurfs", "soundbank",     "soundbanktransient"
        };
        return supported.contains(pool);
    }

    Payload ExportPayload(
        const MemoryReader& read, const std::string& pool, uint64_t address, size_t maxBytes,
        const std::string& profile, const PackageReader& packages
    ) {
        if (!address || address > UINT64_MAX - 48)
            throw std::runtime_error("Invalid payload header address");
        Reader r(read, maxBytes);
        if (pool == "soundbank" || pool == "soundbanktransient") {
            if (profile != "replay-1.20")
                throw std::runtime_error("Sound-bank payload layout is only verified for Replay PC");
            const auto header = r.Bytes(address, 512);
            const auto streamKey = Value<uint64_t>(header, 496);
            if (!streamKey)
                throw PayloadUnavailable("sound_bank_not_loaded", "Sound bank has no loaded StreamKey");
            auto payload = ExportPayload(read, "streamkey", streamKey, maxBytes - header.size(), profile, packages);
            if (payload.data.empty())
                throw PayloadUnavailable("sound_bank_empty", "Sound bank has no loaded SAB bytes");
            MemoryReader bankRead = [&](uint64_t at, void* out, size_t size) {
                if (at > payload.data.size() || size > payload.data.size() - at)
                    return false;
                std::memcpy(out, payload.data.data() + at, size);
                return true;
            };
            sound::Bank bank(bankRead, payload.data.size());
            payload.extension = ".sabl";
            payload.format = "Replay SAB loaded sound bank";
            return payload;
        }
        if (pool == "image") {
            if (profile != "replay-1.20")
                throw std::runtime_error("Image serialization layout is only verified for Replay PC");
            return ExportReplayImage(read, address, maxBytes, packages);
        }
        if (pool == "streamkey" || pool == "xmodelsurfs") {
            if (profile != "replay-1.20")
                throw std::runtime_error("Stream buffer serialization is only verified for Replay PC");
            const bool model = pool == "xmodelsurfs";
            const auto header = r.Bytes(address, model ? 96 : 64);
            const auto key = Value<uint64_t>(header, model ? 16 : 8);
            uint64_t data{};
            size_t size{};
            bool streamed{};
            if (model) {
                // Native 0x13AD700 selects XPakEntryInfo at +16. Shared
                // data size/flags are verified by 0x13B3390/0xE2AFA0.
                const auto shared = Value<uint64_t>(header, 48);
                if (!shared) {
                    if (Value<uint16_t>(header, 56))
                        throw PayloadUnavailable("model_shared_unavailable", "Model surfaces have no shared buffer");
                } else {
                    const auto sharedHeader = r.Bytes(shared, 16);
                    data = Value<uint64_t>(sharedHeader, 0);
                    size = r.Count(Value<uint32_t>(sharedHeader, 8));
                    streamed = Value<uint32_t>(sharedHeader, 12) & 1;
                }
            } else {
                // Native 0xE34050 uses flags+61 bit1 for residency; the
                // XPak info/data/size offsets are +8/+40/+56.
                data = Value<uint64_t>(header, 40);
                size = r.Count(Value<uint32_t>(header, 56));
                streamed = !(Value<uint8_t>(header, 61) & 2);
            }
            Payload result{ model ? ".shared.bin" : ".stream.bin",
                            {},
                            model ? "XSurface shared geometry buffer" : "StreamKey buffer" };
            if (!size)
                return result;
            if (streamed) {
                if (!packages)
                    throw PayloadUnavailable("stream_package_required", "Stream buffer requires XPak input");
                if (!key)
                    throw PayloadUnavailable("stream_key_unavailable", "Stream buffer has no XPak key");
                result.data = packages(key, size);
                if (result.data.size() != size)
                    throw std::runtime_error("Stream package returned an incorrect byte count");
            } else {
                if (!data)
                    throw PayloadUnavailable(
                        "resident_buffer_unavailable",
                        "Nonempty resident stream buffer has no CPU address"
                    );
                result.data = r.Bytes(data, size);
            }
            return result;
        }
        if (pool == "computeshader" || pool == "libshader" || pool == "vertexshader" || pool == "hullshader" ||
            pool == "domainshader" || pool == "pixelshader") {
            if (profile != "replay-1.20" && (profile != "game-test" || pool != "computeshader"))
                throw std::runtime_error("Shader layout not available for profile");
            const auto loadDef = address + (profile == "game-test" ? 32 : 24);
            const auto program = r.Number<uint64_t>(loadDef);
            const auto size = r.Count(r.Number<uint32_t>(loadDef + 8));
            if (!program && !size) {
                Json out{ { "name", r.String(r.Number<uint64_t>(address)) },
                          { "type", pool }, { "program_bytes", 0 },
                          { "status", "empty_shader" },
                          { "flags", r.Number<uint32_t>(loadDef + 12) } };
                return Text(out.dump(2) + '\n', ".shader.json", "empty shader declaration");
            }
            if (!program || !size)
                throw PayloadUnavailable("shader_not_resident", "Shader program bytes are not resident");
            return { ".cso", r.Bytes(program, size), "compiled shader program" };
        }
        if (pool == "rawfile") {
            const size_t compressed = r.Count(r.Number<int32_t>(address + 8));
            const size_t size = r.Count(r.Number<int32_t>(address + 12));
            const auto ptr = r.Number<uint64_t>(address + 16);
            if (!compressed)
                return { "", r.Bytes(ptr, size), "rawfile" };
            if (size > maxBytes - compressed)
                throw std::runtime_error("Decompressed rawfile exceeds budget");
            auto bytes = r.Bytes(ptr, compressed);
            std::vector<uint8_t> out(size ? size : 1);
            uLongf actual = static_cast<uLongf>(out.size());
            uLong consumed = static_cast<uLong>(bytes.size());
            const auto status = uncompress2(out.data(), &actual, bytes.data(), &consumed);
            if (status != Z_OK || actual != size || consumed != bytes.size())
                throw std::runtime_error("Invalid rawfile zlib payload or length");
            out.resize(size);
            return { "", std::move(out), "rawfile" };
        }
        if (pool == "luafile") {
            auto size = r.Count(r.Number<int32_t>(address + 8));
            return { ".lua", r.Bytes(r.Number<uint64_t>(address + 16), size), "lua bytecode or source" };
        }
        if (pool == "ttf") {
            auto size = r.Count(r.Number<int32_t>(address + 8));
            return { ".ttf", r.Bytes(r.Number<uint64_t>(address + 16), size), "TrueType font" };
        }
        if (pool == "scriptfile") {
            uint32_t header[4] = { 0x435347, 0, 0, 0 };
            header[1] = static_cast<uint32_t>(r.Count(r.Number<int32_t>(address + 8)));
            header[2] = static_cast<uint32_t>(r.Count(r.Number<int32_t>(address + 12)));
            header[3] = static_cast<uint32_t>(r.Count(r.Number<int32_t>(address + 16)));
            auto buffer = r.Bytes(r.Number<uint64_t>(address + 24), header[1]);
            auto code = r.Bytes(r.Number<uint64_t>(address + 32), header[3]);
            std::vector<uint8_t> out(sizeof(header));
            std::memcpy(out.data(), header, sizeof(header));
            out.insert(out.end(), buffer.begin(), buffer.end());
            out.insert(out.end(), code.begin(), code.end());
            return { ".gscbin", std::move(out), "gsc-tool gscbin" };
        }
        if (pool == "stringtable") {
            const size_t columns = r.Count(r.Number<int32_t>(address + 8));
            const size_t rows = r.Count(r.Number<int32_t>(address + 12));
            const size_t unique = r.Count(r.Number<int32_t>(address + 16));
            if (columns && rows > maxBytes / 2 / columns)
                throw std::runtime_error("Stringtable dimensions overflow budget");
            auto indices = r.Bytes(r.Number<uint64_t>(address + 24), rows * columns * 2);
            if (unique > maxBytes / 8)
                throw std::runtime_error("Stringtable dictionary exceeds budget");
            auto pointers = r.Bytes(r.Number<uint64_t>(address + 40), unique * 8);
            std::vector<std::string> strings;
            for (size_t i = 0; i < unique; i++) {
                uint64_t ptr{};
                std::memcpy(&ptr, pointers.data() + i * 8, 8);
                strings.push_back(ptr ? r.String(ptr) : "");
            }
            std::string text;
            for (size_t row = 0; row < rows; row++) {
                for (size_t col = 0; col < columns; col++) {
                    uint16_t index{};
                    std::memcpy(&index, indices.data() + (col * rows + row) * 2, 2);
                    if (index >= strings.size())
                        throw std::runtime_error("Stringtable cell references an invalid dictionary index");
                    if (col)
                        text += ',';
                    text += Csv(strings[index]);
                    if (text.size() > maxBytes)
                        throw std::runtime_error("CSV exceeds output budget");
                }
                text += '\n';
            }
            return Text(std::move(text), ".csv", "CSV");
        }
        if (pool == "localize") {
            auto name = r.String(r.Number<uint64_t>(address));
            const auto ptr = r.Number<uint64_t>(address + 8);
            Json out{ { "name", name }, { "value", ptr ? Json(r.String(ptr)) : Json(nullptr) } };
            return Text(out.dump(2, ' ', false, Json::error_handler_t::replace) + '\n', ".json", "localization");
        }
        if (pool == "netconststrings") {
            auto count = r.Count(r.Number<uint32_t>(address + 20));
            if (count > maxBytes / 8)
                throw std::runtime_error("NetConstStrings exceeds budget");
            auto pointers = r.Bytes(r.Number<uint64_t>(address + 24), count * 8);
            Json values = Json::array();
            for (size_t i = 0; i < count; i++) {
                uint64_t ptr{};
                std::memcpy(&ptr, pointers.data() + i * 8, 8);
                values.push_back(ptr ? Json(r.String(ptr)) : Json(nullptr));
            }
            Json out{ { "stringType", r.Number<uint32_t>(address + 8) },
                      { "sourceType", r.Number<uint32_t>(address + 12) },
                      { "flags", r.Number<uint32_t>(address + 16) },
                      { "strings", values } };
            return Text(
                out.dump(2, ' ', false, Json::error_handler_t::replace) + '\n',
                ".json",
                "network constant strings"
            );
        }
        if (pool == "soundbanklist") {
            auto count = r.Number<uint16_t>(address + 8);
            auto bytes = r.Bytes(r.Number<uint64_t>(address + 16), static_cast<size_t>(count) * 4);
            Json hashes = Json::array();
            for (size_t i = 0; i < count; i++) {
                uint32_t value{};
                std::memcpy(&value, bytes.data() + i * 4, 4);
                hashes.push_back(value);
            }
            return Text(hashes.dump(2) + '\n', ".json", "sound bank name hashes");
        }
        throw std::runtime_error("No payload exporter for pool " + pool);
    }
} // namespace tool::mw19::schema
