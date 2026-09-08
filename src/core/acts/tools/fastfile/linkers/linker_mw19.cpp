#include <includes.hpp>
#include <tools/fastfile/fastfile_handlers.hpp>
#include <tools/mw19/mw19_linker.hpp>
#include <tools/mw19/mw19_schema.hpp>
#include <zlib.h>

namespace {
    using namespace fastfile;
    using tool::mw19::schema::Json;
    constexpr size_t Budget = 128 * 1024 * 1024;
    std::vector<uint8_t> ReadInput(const std::filesystem::path& path, size_t& remaining) {
        const auto length = std::filesystem::file_size(path);
        if (length > remaining)
            throw std::runtime_error("IW8 zone input exceeds 128 MiB budget");
        remaining -= static_cast<size_t>(length);
        std::ifstream in(path, std::ios::binary);
        std::vector<uint8_t> bytes(static_cast<size_t>(length));
        if (!in || !in.read(reinterpret_cast<char*>(bytes.data()), bytes.size()))
            throw std::runtime_error("Cannot read IW8 linker input: " + path.string());
        return bytes;
    }
    class IW8Linker final : public FFLinker {
      public:
        IW8Linker(const char* name) : FFLinker(name, "MW2019 Replay 1.20 PC asset linker") {}
        void Link(FastFileLinkerContext& ctx) override {
            if (ctx.opt.platform != XFILE_PC || ctx.opt.encrypt || ctx.opt.m_fd)
                throw std::runtime_error("IW8 linker supports PC unsigned base fastfiles");
            if (!ctx.mainFFName || std::filesystem::path(ctx.mainFFName).filename().string() != ctx.mainFFName)
                throw std::runtime_error("IW8 zone needs a simple >name=value");
            tool::mw19::schema::AssetPath("zone", ctx.mainFFName);
            tool::mw19::schema::Database db(utils::GetProgDir() / "data/mw19/schema.json");
            Json records = Json::array();
            size_t remaining = Budget;
            // Stable order makes identical inputs produce identical output.
            std::vector<std::pair<std::string, zone::AssetData*>> ordered;
            for (auto& [type, entries] : ctx.zone.assets)
                for (auto& entry : entries)
                    ordered.emplace_back(type, &entry);
            std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) {
                return std::pair(a.first, std::string(a.second->value)) <
                       std::pair(b.first, std::string(b.second->value));
            });
            for (auto& [type, entry] : ordered) {
                const std::string id(entry->value);
                const auto input = tool::mw19::schema::AssetPath(ctx.input, id);
                auto bytes = ReadInput(input, remaining);
                Json record;
                if (type == "rawfile" || type == "luafile" || type == "ttf")
                    record = Json{ { "name", id }, { "data", Json::binary(std::move(bytes)) } };
                else if (type == "scriptfile") {
                    if (bytes.size() < 16)
                        throw std::runtime_error("Truncated GSCBIN");
                    uint32_t header[4];
                    std::memcpy(header, bytes.data(), 16);
                    if (header[0] != 0x435347 || uint64_t{ 16 } + header[1] + header[3] != bytes.size() ||
                        header[2] > remaining)
                        throw std::runtime_error("Invalid GSCBIN lengths or magic");
                    remaining -= header[2];
                    std::vector<uint8_t> stack(header[2] ? header[2] : 1);
                    uLongf length = static_cast<uLongf>(stack.size());
                    uLong consumed = header[1];
                    if (header[1] ? uncompress2(stack.data(), &length, bytes.data() + 16, &consumed) != Z_OK ||
                                        length != header[2] || consumed != header[1]
                                  : header[2] != 0)
                        throw std::runtime_error("Invalid GSCBIN compressed stack");
                    stack.resize(header[2]);
                    auto name = id;
                    if (name.ends_with(".gscbin"))
                        name.resize(name.size() - 7);
                    record =
                        Json{ { "name", name },
                              { "stack", Json::binary(std::move(stack)) },
                              { "bytecode",
                                Json::binary(std::vector<uint8_t>(bytes.begin() + 16 + header[1], bytes.end())) } };
                } else
                    record = Json::parse(bytes);
                if (!record.is_object())
                    throw std::runtime_error("IW8 asset descriptor must be an object");
                record["pool"] = type;
                records.push_back(std::move(record));
            }
            auto linked = tool::mw19::linker::Link(db.Profile("replay-1.20"), records);
            auto& ff = ctx.fastfiles.emplace_back();
            ff.ffname = ctx.mainFFName;
            ff.linkedData.assign(linked.body.begin(), linked.body.end());
            std::copy(linked.blocks.begin(), linked.blocks.end(), ff.blockSizes);
            for (auto& pair : ordered)
                pair.second->handled = true;
            LOG_INFO("Linked {} IW8 assets, {} stream bytes", records.size(), ff.linkedData.size());
        }
    };
    class IW8Compressor final : public FFCompressor {
      public:
        IW8Compressor(const char* name) : FFCompressor(name, "MW2019 Replay IWffc100 stored/LZ4 compressor") {}
        void Compress(FastFileLinkerContext& ctx) override {
            if (ctx.opt.platform != XFILE_PC || ctx.opt.encrypt || ctx.opt.m_fd)
                throw std::runtime_error("IW8 compressor supports PC unsigned base fastfiles");
            const std::string compression = ctx.zone.GetConfig("compression", "lz4");
            if (compression != "lz4" && compression != "none")
                throw std::runtime_error("IW8 compression must be lz4 or none");
            for (const auto& ff : ctx.fastfiles) {
                tool::mw19::linker::Zone zone;
                zone.body.assign(ff.linkedData.begin(), ff.linkedData.end());
                std::copy_n(ff.blockSizes, 11, zone.blocks.begin());
                auto output = tool::mw19::linker::Pack(
                    zone,
                    compression == "lz4",
                    ctx.opt.chunkSize ? ctx.opt.chunkSize : 0x10000
                );
                const auto path =
                    tool::mw19::schema::AssetPath(ctx.opt.m_output / "zone", std::string(ff.ffname) + ".ff");
                if (std::filesystem::exists(path)) {
                    if (std::filesystem::equivalent(path, ctx.zoneFile))
                        throw std::runtime_error("IW8 output would overwrite the zone input");
                    for (const auto& [type, entries] : ctx.zone.assets)
                        for (const auto& entry : entries)
                            if (std::filesystem::equivalent(
                                    path,
                                    tool::mw19::schema::AssetPath(ctx.input, entry.value)
                                ))
                                throw std::runtime_error("IW8 output would overwrite an asset input");
                }
                // Write only after every asset linked and the complete container encoded.
                std::filesystem::create_directories(path.parent_path());
                auto temporary = path;
                temporary += std::format(".{}.tmp", GetCurrentProcessId());
                HANDLE file = CreateFileW(
                    temporary.c_str(),
                    GENERIC_WRITE,
                    0,
                    nullptr,
                    CREATE_NEW,
                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
                    nullptr
                );
                if (file == INVALID_HANDLE_VALUE)
                    throw std::runtime_error("Cannot create exclusive IW8 output temporary file");
                DWORD written{};
                const bool complete =
                    ::WriteFile(file, output.data(), static_cast<DWORD>(output.size()), &written, nullptr) &&
                    written == output.size();
                CloseHandle(file);
                if (!complete) {
                    DeleteFileW(temporary.c_str());
                    throw std::runtime_error("Cannot write complete IW8 fastfile");
                }
                if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                    DeleteFileW(temporary.c_str());
                    throw std::runtime_error("Cannot finalize IW8 fastfile");
                }
                LOG_INFO("Wrote {} ({} bytes, unsigned Replay xfile 0xff7)", path.string(), output.size());
            }
        }
    };
    utils::ArrayAdder<IW8Linker, FFLinker> iw8Linker{ GetLinkers(), "IW8" }, mw19Linker{ GetLinkers(), "MW19" };
    utils::ArrayAdder<IW8Compressor, FFCompressor> iw8Compressor{ GetCompressors(), "IW8" },
        mw19Compressor{ GetCompressors(), "MW19" };
} // namespace
