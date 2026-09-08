#include <includes.hpp>
#include <tools/fastfile/fastfile_handlers.hpp>
#include <tools/mw19/mw19_ff_stream.hpp>
#include <tools/mw19/mw19_replay_bindings.hpp>
#include <tools/mw19/mw19_payload.hpp>
#include <tools/mw19/mw19_mesh.hpp>
#include <tools/mw19/mw19_xpak.hpp>
#include <hook/memory.hpp>
#include <deps/oodle.hpp>
#include <core/hashes/hash_names.hpp>
#include <zlib.h>

namespace fastfile::handlers::mw19_replay {
    namespace {
        using namespace tool::mw19;
        using namespace tool::mw19::schema;
        struct Asset {
            uint32_t type, padding;
            void* header;
        };
        struct AssetList {
            uint32_t stringsCount, stringsLoaded;
            char** strings;
            uint32_t count, loaded;
            Asset* assets;
        };
        static_assert(sizeof(AssetList) == 32 && sizeof(Asset) == 16);
        struct Record {
            uint32_t type;
            void* header;
        };
        struct State {
            hook::module_mapper::Module module{ true };
            std::unique_ptr<Database> database;
            std::unique_ptr<replay::Streams> streams;
            FastFileOption* opt{};
            FastFileContext* ctx{};
            uint8_t** cursor{};
            std::vector<std::pair<uintptr_t, size_t>> owned;
            std::vector<Record> records;
            size_t ownedBytes{};
            std::array<size_t, 117> sampled{};
            std::set<std::string> selected;
            core::hashes::names::NamesStore names{ [](const char* name) { return hash::HashIWAsset(name); } };
            std::vector<xpak::Archive> packages;
            deps::oodle::Oodle oodle;
            xpak::OodleDecoder decode;
            std::filesystem::path out;
            Json report;
            std::ofstream journal;
            size_t current{};
            bool Read(uint64_t address, void* destination, size_t size) {
                bool valid = streams && streams->Contains(address, size);
                if (!valid) {
                    // Sorted once loading finishes; export performs many
                    // field reads against these retained root allocations.
                    auto it =
                        std::upper_bound(owned.begin(), owned.end(), address, [](uint64_t value, const auto& range) {
                            return value < range.first;
                        });
                    if (it != owned.begin()) {
                        auto [start, length] = *--it;
                        valid = address - start <= length && size <= length - (address - start);
                    }
                }
                return valid && hook::memory::ReadMemorySafe(reinterpret_cast<void*>(address), destination, size);
            }
            void Progress() {
                report["loaded_assets"] = records.size();
                report["serialized_bytes_read"] = streams ? streams->Consumed() : 0;
                report["top_level_index"] = current;
                WriteJsonAtomic(out / "manifest.json", report);
            }
        } state;
        void LoadStream(int start, void* destination, size_t size) {
            state.streams->Load(start == 0, destination, size);
        }
        void PushStream(uint32_t block) { state.streams->Push(block); }
        void PopStream() { state.streams->Pop(); }
        void AlignStream(size_t mask) { state.streams->Align(mask); }
        void BeginAsset(int, void*) { state.streams->BeginAsset(); }
        void EndAsset() { state.streams->EndAsset(); }
        void SharedPush() { state.streams->SharedPush(); }
        void SharedPop() { state.streams->SharedPop(); }
        void String(char** value) { state.streams->String(value); }
        void Resolve(uint64_t* value) { *value = reinterpret_cast<uintptr_t>(state.streams->Resolve(*value, false)); }
        void Alias(uint64_t* value) { *value = reinterpret_cast<uintptr_t>(state.streams->Resolve(*value, true)); }
        void** Insert() { return state.streams->Insert(); }
        const void* Temporary(size_t length, size_t alignment) { return state.streams->Temporary(length, alignment); }
        uintptr_t RuntimeOnly() { return 0; }
        void ScriptString(uint32_t*) {} // Preserve the fastfile's script-string indices.

        template<size_t index>
        void Link(void** header) {
            constexpr auto binding = replay::bindings[index];
            const auto& pool = state.database->Profile("replay-1.20").at("pools").at(binding.type);
            size_t size = pool.at("size");
            if (!header || !*header || !state.streams->Contains(reinterpret_cast<uintptr_t>(*header), size))
                throw std::runtime_error("Replay asset header outside loader memory");
            if (size > 128 * 1024 * 1024 - state.ownedBytes || state.records.size() >= 1000000)
                throw std::runtime_error("Replay asset header budget exceeded");
            void* copy = state.ctx->zoneMemory.AllocAligned<byte>(size, 16);
            std::memcpy(copy, *header, size);
            state.owned.emplace_back(reinterpret_cast<uintptr_t>(copy), size);
            state.ownedBytes += size;
            *header = copy;
            state.records.push_back({ binding.type, copy });
        }
        template<size_t... I>
        void InstallLinks(std::index_sequence<I...>) {
            (state.module->Redirect(size_t(replay::bindings[I].link), &Link<I>), ...);
        }
        // Keep hardware faults from leaving an apparently successful capability report.
        int CallNative(void (*load)(int), int start, uintptr_t* fault) {
            __try {
                load(start);
                return 0;
            } __except (
                GetExceptionCode() == 0xe06d7363
                    ? EXCEPTION_CONTINUE_SEARCH
                    : (*fault =
                           reinterpret_cast<uintptr_t>(GetExceptionInformation()->ExceptionRecord->ExceptionAddress),
                       EXCEPTION_EXECUTE_HANDLER)
            ) {
                return GetExceptionCode();
            }
        }
        void NativeLoad(uint32_t type, void** header) {
            auto it = std::find_if(std::begin(replay::bindings), std::end(replay::bindings), [&](const auto& binding) {
                return binding.type == type;
            });
            if (it == std::end(replay::bindings))
                throw std::runtime_error("Replay asset has no enabled loader: " + std::to_string(type));
            *reinterpret_cast<void***>((*state.module)[it->global]) = header;
            uintptr_t fault{};
            int code = CallNative(reinterpret_cast<void (*)(int)>((*state.module)[it->loader]), 1, &fault);
            if (code)
                throw std::runtime_error(
                    std::format("Replay native loader fault 0x{:x} at 0x{:x}, type {}", uint32_t(code), fault, type)
                );
        }

        void ExportRecord(const Record& record) {
            const auto& profile = state.database->Profile("replay-1.20");
            const auto& pool = profile.at("pools").at(record.type);
            std::string type = pool.at("name");
            if (!state.selected.empty() && !state.selected.contains(type))
                return;
            if (state.opt->replayTest && state.sampled[record.type] >= state.opt->replayLimit)
                return;
            ++state.sampled[record.type];
            Json item{ { "type", type }, { "type_id", record.type }, { "status", "pending" } };
            MemoryReader read = [&](uint64_t a, void* b, size_t n) { return state.Read(a, b, n); };
            try {
                uint64_t nameAddress{};
                if (!read(
                        reinterpret_cast<uintptr_t>(record.header) + pool.at("name_offset").get<size_t>(),
                        &nameAddress,
                        8
                    ))
                    throw std::runtime_error("Cannot read asset name pointer");
                auto name = ReadString(read, nameAddress);
                item["name"] = name;
                if (!state.names.Contains(hash::HashIWAsset(name.c_str()), true)) {
                    --state.sampled[record.type];
                    return;
                }
                if (!name.empty() && name[0] == ',') {
                    state.report["references"] = state.report.at("references").get<size_t>() + 1;
                    --state.sampled[record.type];
                    return; // A name-only dependency does not exercise an exporter.
                } else if (state.opt->noAssetDump) {
                    item["status"] = "inventory";
                } else {
                    PackageReader packages = [&](uint64_t key, size_t size) {
                        for (size_t i = state.packages.size(); i-- > 0;) {
                            if (!state.packages[i].Find(key))
                                continue;
                            try {
                                auto bytes = state.packages[i].Extract(key, size, state.decode);
                                item["package_reads"].push_back(
                                    Json{ { "key", key },
                                          { "bytes", bytes.size() },
                                          { "archive", state.opt->replayPackages[i].generic_string() },
                                          { "crc32", crc32(0, bytes.data(), static_cast<uInt>(bytes.size())) } }
                                );
                                return bytes;
                            } catch (const xpak::DecoderUnavailable& e) {
                                throw PayloadUnavailable("package_decoder_unavailable", e.what());
                            }
                        }
                        item["package_missing"].push_back(Json{ { "key", key }, { "expected_bytes", size } });
                        throw PayloadUnavailable(
                            "package_key_missing",
                            "Required payload is absent from supplied XPaks"
                        );
                    };
                    Payload payload;
                    if (HasPayloadExporter(type)) {
                        payload = ExportPayload(
                            read,
                            type,
                            reinterpret_cast<uintptr_t>(record.header),
                            128 * 1024 * 1024,
                            "replay-1.20",
                            packages
                        );
                    } else {
                        Limits limits;
                        limits.maxBytes = 256 * 1024 * 1024;
                        limits.maxNodes = 4000000;
                        limits.maxArray = 32000000;
                        limits.maxDepth = 64;
                        limits.compactArrays = true;
                        Inspector inspector(*state.database, read, limits, "replay-1.20");
                        auto inspection =
                            inspector.Inspect(pool.at("root_type"), reinterpret_cast<uintptr_t>(record.header));
                        item["field_issues"] = inspection.at("issues");
                        item["field_nodes"] = inspection.at("nodes");
                        item["field_bytes_read"] = inspection.at("bytes_read");
                        for (auto key : { "read_errors",
                                          "unresolved_pointers",
                                          "unresolved_unions",
                                          "external_payloads",
                                          "runtime_references" })
                            item[key] = inspection.at(key);
                        payload = ExportStructuredAsset(profile, pool, std::move(inspection), limits.maxBytes);
                    }
                    item["payload_bytes"] = payload.data.size();
                    item["payload_crc32"] = crc32(0, payload.data.data(), static_cast<uInt>(payload.data.size()));
                    item["format"] = payload.format;
                    GeometryPayload geometry;
                    if (state.opt->replayGeometry && type == "xmodelsurfs") {
                        item["geometry_scope"] = "base surface geometry";
                        item["geometry_status"] = "pending";
                        geometry = ExportReplayGeometry(read, reinterpret_cast<uintptr_t>(record.header), payload.data);
                        item["geometry_surfaces"] = geometry.surfaces;
                        item["geometry_vertices"] = geometry.vertices;
                        item["geometry_triangles"] = geometry.triangles;
                        item["geometry_bytes"] = geometry.payload.data.size();
                        item["geometry_crc32"] =
                            crc32(0, geometry.payload.data.data(), static_cast<uInt>(geometry.payload.data.size()));
                        item["geometry_status"] = "ok";
                    }
                    if (!state.opt->replayTest) {
                        auto root = state.out / "assets" / type;
                        std::filesystem::path path;
                        try {
                            path = AssetPath(root, name);
                        } catch (const std::exception&) {
                            path = root / "_encoded" / std::to_string(state.report.at("tested").get<size_t>());
                            item["filename_encoded"] = true;
                        }
                        path += "." + std::to_string(state.report.at("tested").get<size_t>()) + payload.extension;
                        std::filesystem::create_directories(path.parent_path());
                        if (!utils::WriteFile(path, payload.data.data(), payload.data.size()))
                            throw std::runtime_error("Cannot write Replay asset");
                        item["file"] = path.lexically_relative(state.out).generic_string();
                        if (!geometry.payload.data.empty()) {
                            auto meshPath = path;
                            meshPath += geometry.payload.extension;
                            if (!utils::WriteFile(
                                    meshPath,
                                    geometry.payload.data.data(),
                                    geometry.payload.data.size()
                                )) {
                                item["geometry_status"] = "failed";
                                throw std::runtime_error("Cannot write Replay geometry");
                            }
                            item["geometry_file"] = meshPath.lexically_relative(state.out).generic_string();
                        }
                    }
                    item["status"] = "ok";
                }
            } catch (const PayloadUnavailable& e) {
                if (item.contains("geometry_status") && item["geometry_status"] == "pending")
                    item["geometry_status"] = "unavailable";
                item["status"] = "unavailable";
                item["reason"] = e.reason;
                item["error"] = e.what();
                state.report["unavailable"] = state.report.at("unavailable").get<size_t>() + 1;
            } catch (const std::exception& e) {
                if (item.contains("geometry_status") && item["geometry_status"] == "pending")
                    item["geometry_status"] = "failed";
                item["status"] = "failed";
                item["error"] = e.what();
                state.report["failed"] = state.report.at("failed").get<size_t>() + 1;
            }
            state.report["tested"] = state.report.at("tested").get<size_t>() + 1;
            state.journal << item.dump(-1, ' ', false, Json::error_handler_t::replace) << '\n';
            state.journal.flush();
            if (!state.journal)
                throw std::runtime_error("Cannot write Replay asset report");
        }

        void ExportScriptStrings(const AssetList& list) {
            state.report["script_strings"] = list.stringsCount;
            if (state.opt->noAssetDump)
                return;
            Json strings = Json::array();
            size_t total{};
            MemoryReader read = [&](uint64_t a, void* b, size_t n) { return state.Read(a, b, n); };
            for (size_t i = 0; i < list.stringsCount; ++i) {
                if (!list.strings[i]) {
                    strings.push_back(nullptr);
                    continue;
                }
                auto value = ReadString(read, reinterpret_cast<uintptr_t>(list.strings[i]));
                if (value.size() > 32 * 1024 * 1024 - total)
                    throw std::runtime_error("Replay script strings exceed 32 MiB");
                total += value.size();
                try {
                    static_cast<void>(Json(value).dump());
                    strings.push_back(value);
                } catch (const Json::type_error&) {
                    static constexpr char digits[] = "0123456789abcdef";
                    std::string hex;
                    for (unsigned char c : value) {
                        hex += digits[c >> 4];
                        hex += digits[c & 15];
                    }
                    strings.push_back(Json{ { "string_bytes", hex } });
                }
            }
            auto encoded = strings.dump();
            state.report["script_strings_crc32"] =
                crc32(0, reinterpret_cast<const Bytef*>(encoded.data()), static_cast<uInt>(encoded.size()));
            if (!state.opt->replayTest) {
                WriteJsonAtomic(state.out / "script_strings.json", strings);
                state.report["script_strings_file"] = "script_strings.json";
            }
        }

        class Handler : public FFHandler {
          public:
            Handler()
                : FFHandler(
                      "mw19replay", "Modern Warfare 2019 1.20 Replay",
                      compatibility::scobalula::csi::CordycepGame::CG_MW4, true
                  ) {}
            void Init(FastFileOption& opt) override {
                state.opt = &opt;
                state.packages.clear();
                state.decode = {};
                state.database = std::make_unique<Database>(utils::GetProgDir() / "data/mw19/schema.json");
                if (!opt.game)
                    throw std::runtime_error("mw19replay requires -g with the 1.20 Replay executable");
                if (!state.module.Load(opt.game, false))
                    throw std::runtime_error("Cannot map Replay executable");
                auto& mod = *state.module;
                if (mod.ModuleInformation().SizeOfImage() != 0x1324b000)
                    throw std::runtime_error("Executable image size does not match Replay 1.20");
                for (const auto& binding : replay::bindings)
                    if (std::memcmp(mod[binding.loader], binding.signature.data(), binding.signature.size()))
                        throw std::runtime_error("Executable does not match Replay 1.20 loader signatures");
                MemoryReader native = [&](uint64_t a, void* b, size_t n) {
                    return hook::memory::ReadMemorySafe(reinterpret_cast<void*>(a), b, n);
                };
                ValidateProfile(
                    native,
                    reinterpret_cast<uintptr_t>(mod[size_t(0)]),
                    state.database->Profile("replay-1.20")
                );
                state.cursor = reinterpret_cast<uint8_t**>(mod[0xd120c00]);
                mod.Redirect(size_t(0x11b2a20), LoadStream);
                mod.Redirect(size_t(0x11b2570), PushStream);
                mod.Redirect(size_t(0x11b24c0), PopStream);
                mod.Redirect(size_t(0xd8d480), AlignStream);
                mod.Redirect(size_t(0xd8da80), BeginAsset);
                mod.Redirect(size_t(0xd8d850), EndAsset);
                mod.Redirect(size_t(0xd8e5b0), SharedPush);
                mod.Redirect(size_t(0xd8e2e0), SharedPop);
                mod.Redirect(size_t(0xd904c0), Alias);
                mod.Redirect(size_t(0xd90530), Resolve);
                mod.Redirect(size_t(0x11b2390), Insert);
                mod.Redirect(size_t(0x11b2b00), String);
                mod.Redirect(size_t(0x11b29b0), Temporary);
                mod.Redirect(size_t(0x11b2ca0), ScriptString);
                mod.Redirect(size_t(0x11b2680), RuntimeOnly);
                mod.Redirect(size_t(0xf669f0), RuntimeOnly);
                for (auto address : replay::runtimeHooks)
                    mod.Redirect(size_t(address), RuntimeOnly);
                InstallLinks(std::make_index_sequence<std::size(replay::bindings)>{});
                state.selected.clear();
                state.names.LoadConfig(opt.assets);
                if (opt.assetTypes) {
                    std::string selection{ opt.assetTypes };
                    std::replace(selection.begin(), selection.end(), ',', ' ');
                    std::istringstream tokens(selection);
                    for (std::string type; tokens >> type;) {
                        state.database->Pool(state.database->Profile("replay-1.20"), type);
                        state.selected.insert(type);
                    }
                }
                size_t indexBytes{};
                for (auto& path : opt.replayPackages) {
                    auto archive = xpak::Archive::Open(path);
                    indexBytes += archive.Entries().size() * sizeof(xpak::Entry);
                    if (indexBytes > 128 * 1024 * 1024)
                        throw std::runtime_error("Replay XPak index budget exceeded");
                    state.packages.push_back(std::move(archive));
                }
                if (!opt.replayOodle.empty()) {
                    if (!opt.replayOodle.is_absolute() || !state.oodle.LoadOodle(opt.replayOodle.string().c_str()))
                        throw std::runtime_error("Cannot load absolute Oodle DLL path");
                    state.decode = [](std::span<const uint8_t> input, std::span<uint8_t> output) {
                        return state.oodle.Decompress(
                                   input.data(),
                                   static_cast<uint32_t>(input.size()),
                                   output.data(),
                                   static_cast<uint32_t>(output.size()),
                                   deps::oodle::OODLE_FS_YES
                               ) == static_cast<int32_t>(output.size());
                    };
                }
            }
            void Cleanup() override {
                state.streams.reset();
                state.records.clear();
                state.owned.clear();
                state.packages.clear();
                state.journal.close();
                state.database.reset();
                state.module.Free();
            }
            void Handle(FastFileOption& opt, core::bytebuffer::ByteBuffer& reader, FastFileContext& ctx) override {
                state.streams.reset();
                state.ctx = &ctx;
                state.records.clear();
                state.owned.clear();
                state.ownedBytes = 0;
                state.sampled.fill(0);
                state.current = 0;
                state.out = opt.m_output / "mw19replay" / ctx.ffname;
                std::filesystem::create_directories(state.out);
                state.report =
                    Json{ { "profile", "replay-1.20" }, { "fastfile", ctx.file },        { "complete", false },
                          { "success", false },         { "test_only", opt.replayTest }, { "tested", size_t(0) },
                          { "failed", size_t(0) },      { "unavailable", size_t(0) },    { "references", size_t(0) },
                          { "journal", "assets.jsonl" } };
                state.journal.close();
                state.journal.clear();
                state.journal.open(state.out / "assets.jsonl", std::ios::binary | std::ios::trunc);
                if (!state.journal)
                    throw std::runtime_error("Cannot create Replay asset report");
                try {
                    if (ctx.blocksCount != 11 || opt.replayFileVersion != 0xff7)
                        throw std::runtime_error("Expected Replay 0xff7 with eleven header reservations");
                    std::array<std::span<uint8_t>, 8> blocks;
                    size_t total{};
                    for (size_t i = 0; i < blocks.size(); ++i) {
                        size_t size = ctx.blockSizes[i].size;
                        if (size > 2ull * 1024 * 1024 * 1024 - total)
                            throw std::runtime_error("Replay stream reservations exceed 2 GiB");
                        total += size;
                        auto data = ctx.zoneMemory.AllocAligned<byte>(std::max<size_t>(size, 1), 0x100000);
                        std::memset(data, 0, size);
                        blocks[i] = { data, size };
                    }
                    state.streams = std::make_unique<replay::Streams>(
                        blocks,
                        std::span<const uint8_t>(reader.Ptr<byte>(), reader.Remaining()),
                        *state.cursor
                    );
                    state.Progress();
                    AssetList list;
                    auto header = state.streams->Take(sizeof(list));
                    std::memcpy(&list, header.data(), sizeof(list));
                    if (list.count > 1000000 || list.stringsCount > 1000000)
                        throw std::runtime_error("Replay asset/string count exceeds limit");
                    if (list.count && !list.assets)
                        throw std::runtime_error("Replay asset list has null storage");
                    state.streams->Push(5);
                    if (list.strings) {
                        state.streams->Align(7);
                        list.strings = reinterpret_cast<char**>(*state.cursor);
                        state.streams->Load(true, list.strings, size_t(list.stringsCount) * 8);
                        auto global = reinterpret_cast<char***>((*state.module)[0x5d417c0]);
                        for (size_t i = 0; i < list.stringsCount; ++i) {
                            *global = &list.strings[i];
                            uintptr_t fault{};
                            int code =
                                CallNative(reinterpret_cast<void (*)(int)>((*state.module)[0xdd3400]), 1, &fault);
                            if (code)
                                throw std::runtime_error(
                                    std::format(
                                        "Replay script-string loader fault 0x{:x} at 0x{:x}",
                                        uint32_t(code),
                                        fault
                                    )
                                );
                        }
                    } else if (list.stringsCount)
                        throw std::runtime_error("Replay script-string list has null storage");
                    ctx.scrStrings = const_cast<const char**>(list.strings);
                    ctx.scrStringsCount = list.stringsCount;
                    state.streams->Align(7);
                    list.assets = reinterpret_cast<Asset*>(*state.cursor);
                    state.streams->Load(true, list.assets, size_t(list.count) * sizeof(Asset));
                    for (; state.current < list.count; ++state.current) {
                        auto& asset = list.assets[state.current];
                        NativeLoad(asset.type, &asset.header);
                        if (!(state.current % 256))
                            state.Progress();
                    }
                    state.streams->Pop();
                    state.streams->Finish();
                    std::sort(state.owned.begin(), state.owned.end());
                    ExportScriptStrings(list);
                    auto& coverage = state.report["loaded_by_type"] = Json::object();
                    for (const auto& record : state.records) {
                        std::string type =
                            state.database->Profile("replay-1.20").at("pools").at(record.type).at("name");
                        coverage[type] = coverage.value(type, size_t(0)) + 1;
                    }
                    state.Progress();
                    for (size_t i = 0; i < state.records.size(); ++i) {
                        ExportRecord(state.records[i]);
                        state.report["export_record_index"] = i + 1;
                        if (!((i + 1) % 128))
                            state.Progress();
                    }
                    state.report["complete"] = true;
                    state.report["success"] =
                        !state.report.at("failed").get<size_t>() && !state.report.at("unavailable").get<size_t>();
                    state.Progress();
                    state.journal.close();
                    LOG_INFO(
                        "Replay: {} assets loaded, {} tested, {} failed, {} unavailable",
                        state.records.size(),
                        state.report.at("tested").get<size_t>(),
                        state.report.at("failed").get<size_t>(),
                        state.report.at("unavailable").get<size_t>()
                    );
                    if (state.report.at("failed").get<size_t>() || state.report.at("unavailable").get<size_t>())
                        throw std::runtime_error(
                            "Replay asset export has failures or unavailable payloads; see manifest.json"
                        );
                } catch (const std::exception& error) {
                    state.report["error"] = error.what();
                    state.Progress();
                    state.journal.close();
                    throw;
                }
                state.streams.reset();
            }
        };
        utils::ArrayAdder<Handler, FFHandler> registration{ GetHandlers() };
    } // namespace
} // namespace fastfile::handlers::mw19_replay
