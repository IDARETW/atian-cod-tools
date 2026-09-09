#include <includes.hpp>
#include <tools/fastfile/fastfile_handlers.hpp>
#include <tools/mw19/mw19_ff_stream.hpp>
#include <tools/mw19/mw19_replay_bindings.hpp>
#include <tools/mw19/mw19_payload.hpp>
#include <tools/mw19/mw19_mesh.hpp>
#include <tools/mw19/mw19_xpak.hpp>
#include <hook/memory.hpp>
#include <hook/process.hpp>
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
            size_t retainedPixelBytes{};
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
            uint32_t lastAssetType{};
            void* lastAssetRoot{};
            uint64_t legacyFrustumMeshes{};
            std::array<uint8_t, 16> frustumLoaderPrologue{};
            uintptr_t legacyViewStart{};
            size_t legacyViewBytes{};
            Json pendingLegacyWorld;
            std::map<void*, Json> serializedLayouts;
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
        std::string HexBytes(const void* source, size_t size) {
            static constexpr char digits[] = "0123456789abcdef";
            const auto bytes = static_cast<const uint8_t*>(source);
            std::string result(size * 2, '\0');
            for (size_t i = 0; i < size; ++i) {
                result[i * 2] = digits[bytes[i] >> 4]; result[i * 2 + 1] = digits[bytes[i] & 15];
            }
            return result;
        }
        void* Retained(size_t bytes, size_t alignment = 16);
        void LoadStream(int start, void* destination, size_t size) {
            if (start == 0 && destination == state.lastAssetRoot) {
                state.report["last_asset_load"] = Json{{"type", state.lastAssetType}, {"bytes", size},
                                                       {"offset", state.streams->Consumed()}};
            }
            if (start == 0 && size == 17808 && state.opt->replayFileVersion == 0xfda &&
                destination == *reinterpret_cast<void**>((*state.module)[0x5d40d00])) {
                state.streams->Load(true, destination, 17744);
                state.streams->Advance(64);
                auto data = static_cast<uint8_t*>(destination);
                std::array<uint8_t, 17744> old;
                std::memcpy(old.data(), data, old.size());
                state.pendingLegacyWorld = Json{{"xfile_version", 0xfda}, {"root_size", old.size()},
                    {"root_encoding", "unrelocated-hex"}, {"root_bytes", HexBytes(old.data(), old.size())},
                    {"normalized_layout", "Replay::GfxWorld"}};
                std::memset(data, 0, 17808);
                auto copy = [&](size_t to, size_t from, size_t bytes) { std::memcpy(data + to, old.data() + from, bytes); };
                // FDA has eight sort keys, an 896-byte dynamic lightset,
                // 31 visibility views and the legacy tessellation-view array.
                copy(0, 0, 92); copy(120, 92, 24); copy(144, 120, 14336);
                if (std::any_of(old.begin() + 14456, old.begin() + 15352, [](uint8_t b) { return b != 0; }))
                    throw std::runtime_error("Populated FDA dynamic-lightset layout requires additional mapping");
                copy(15408, 15352, 112);
                std::memcpy(&state.legacyFrustumMeshes, old.data() + 15464, 8);
                copy(15712, 15472, 472);
                copy(16192, 15944, 88);
                copy(16280, 16032, 24);
                copy(16304, 16056, 31 * 8);
                copy(16568, 16304, 31 * 8);
                copy(16832, 16552, 32);
                copy(16864, 16832, 48);
                copy(16912, 16880, 32);
                copy(16944, 16912, 31 * 8);
                copy(17208, 17160, 31 * 8);
                copy(17472, 17408, 336);
                return;
            }
            if (start == 0 && size == 120 && state.opt->replayFileVersion == 0xfda &&
                destination == *reinterpret_cast<void**>((*state.module)[0x5d41208])) {
                // FDA ScriptableDef has no network-LOD override fields at
                // +92/+96. Normalize its 112 bytes before the native fixups.
                state.streams->Load(true, destination, 112);
                auto data = static_cast<uint8_t*>(destination);
                state.streams->Advance(8);
                std::memmove(data + 100, data + 92, 20);
                std::memset(data + 92, 0, 8);
                return;
            }
            if (start == 0 && state.opt->replayFileVersion == 0xfda &&
                reinterpret_cast<uintptr_t>(_ReturnAddress()) - reinterpret_cast<uintptr_t>((*state.module)[size_t(0)]) == 0xd93220) {
                size_t count = size / 32;
                if (size % 32 || count > 65536) throw std::runtime_error("Invalid FDA view-frustum extent");
                auto raw = static_cast<uint8_t*>(destination);
                state.streams->Load(true, raw, count * 48);
                auto normalized = static_cast<uint8_t*>(Retained(size));
                state.legacyViewStart = reinterpret_cast<uintptr_t>(normalized);
                state.legacyViewBytes = size;
                state.pendingLegacyWorld["view_frustums"] = Json{{"stride", 48}, {"headers", HexBytes(raw, count * 48)}};
                for (size_t i = 0; i < count; ++i) {
                    for (size_t field = 0; field < 3; ++field) {
                        uint32_t amount{}; std::memcpy(&amount, raw + i * 48 + field * 16, 4);
                        if (amount > UINT8_MAX) throw std::runtime_error("FDA view-frustum count exceeds normalized field");
                        normalized[i * 32 + field] = amount;
                        std::memcpy(normalized + i * 32 + 8 + field * 8, raw + i * 48 + 8 + field * 16, 8);
                    }
                }
                *reinterpret_cast<void**>((*state.module)[0x5d40c70]) = normalized;
                auto world = *reinterpret_cast<uint8_t**>((*state.module)[0x5d40d00]);
                std::memcpy(world + 15712, &normalized, 8);
                return;
            }
            if (start == 0 && state.opt->replayFileVersion == 0xfda) {
                auto frustum = *reinterpret_cast<uint8_t**>((*state.module)[0x5d40c70]);
                auto address = reinterpret_cast<uintptr_t>(frustum);
                if (state.legacyViewBytes >= 32 && address >= state.legacyViewStart &&
                    address - state.legacyViewStart <= state.legacyViewBytes - 32 && state.streams->Block() == 5) {
                    void* indices{}; std::memcpy(&indices, frustum + 16, 8);
                    if (destination == indices && destination == *reinterpret_cast<void**>((*state.module)[0x5d415d8]) && size == frustum[1]) {
                        auto source = static_cast<uint16_t*>(destination);
                        state.streams->Load(true, source, size * 2);
                        auto normalized = static_cast<uint8_t*>(Retained(size));
                        for (size_t i = 0; i < size; ++i) {
                            if (source[i] >= frustum[2]) throw std::runtime_error("FDA light view index outside vertices");
                            normalized[i] = source[i];
                        }
                        std::memcpy(frustum + 16, &normalized, 8);
                        *reinterpret_cast<void**>((*state.module)[0x5d415d8]) = normalized;
                        return;
                    }
                }
            }
            try { state.streams->Load(start == 0, destination, size); }
            catch (...) {
                state.report["failed_stream_read"] = Json{
                    {"bytes", size}, {"block", state.streams->Block()},
                    {"caller_rva", reinterpret_cast<uintptr_t>(_ReturnAddress()) - reinterpret_cast<uintptr_t>((*state.module)[size_t(0)])}};
                throw;
            }
        }
        void PushStream(uint32_t block) { state.streams->Push(block); }
        void PopStream() { state.streams->Pop(); }
        void AlignStream(size_t mask) { state.streams->Align(mask); }
        void BeginAsset(int type, void* root) {
            state.lastAssetType = type; state.lastAssetRoot = root;
            state.streams->BeginAsset();
        }
        void EndAsset() { state.streams->EndAsset(); }
        void SharedPush() { state.streams->SharedPush(); }
        void SharedPop() { state.streams->SharedPop(); }
        void String(char** value) { state.streams->String(value); }
        void Resolve(uint64_t* value) {
            try { *value = reinterpret_cast<uintptr_t>(state.streams->Resolve(*value, false)); }
            catch (...) { state.report["failed_packed_pointer"] = *value; throw; }
        }
        void Alias(uint64_t* value) {
            try { *value = reinterpret_cast<uintptr_t>(state.streams->Resolve(*value, true)); }
            catch (...) { state.report["failed_alias"] = *value; throw; }
        }
        void** Insert() { return state.streams->Insert(); }
        const void* Temporary(size_t length, size_t alignment) { return state.streams->Temporary(length, alignment); }
        uintptr_t RuntimeOnly() { return 0; }
        void RetainImagePixels(uint8_t** pixels, const uint8_t* image) {
            // Load_GfxImagePixels uses rewindable stream 1. Capture at the
            // upload boundary, before DB_PopStreamPos reuses those bytes.
            uint32_t size{};
            if (!image || !state.streams->Contains(reinterpret_cast<uintptr_t>(image), 232))
                throw std::runtime_error("Image upload header outside loader memory");
            std::memcpy(&size, image + 28, 4);
            if (!pixels || !*pixels || !size) return;
            if (size > 512ull * 1024 * 1024 - state.retainedPixelBytes ||
                !state.streams->Contains(reinterpret_cast<uintptr_t>(*pixels), size))
                throw std::runtime_error("Resident image pixels exceed retained payload bounds");
            auto copy = state.ctx->zoneMemory.AllocAligned<uint8_t>(size, 16);
            std::memcpy(copy, *pixels, size);
            state.owned.emplace_back(reinterpret_cast<uintptr_t>(copy), size);
            state.retainedPixelBytes += size;
            *pixels = copy;
        }
        void ScriptString(uint32_t*) {} // Preserve the fastfile's script-string indices.

        void* Retained(size_t bytes, size_t alignment) {
            if (bytes > 512ull * 1024 * 1024 - state.retainedPixelBytes)
                throw std::runtime_error("Retained loader payload budget exceeded");
            auto result = state.ctx->zoneMemory.AllocAligned<uint8_t>(std::max(size_t(1), bytes), alignment);
            std::memset(result, 0, bytes);
            state.owned.emplace_back(reinterpret_cast<uintptr_t>(result), bytes);
            state.retainedPixelBytes += bytes;
            return result;
        }
        void LegacyFrustumLights(int start) {
            if (state.opt->replayFileVersion != 0xfda)
                throw std::runtime_error("Legacy frustum adapter used for another revision");
            auto world = *reinterpret_cast<uint8_t**>((*state.module)[0x5d40d00]);
            auto output = world + 15520;
            if (!state.legacyFrustumMeshes) return;
            uint32_t count{}; std::memcpy(&count, world + 24, 4);
            if (count > 65536) throw std::runtime_error("FDA light mesh count exceeds bounds");
            state.streams->Align(7);
            auto meshes = *state.cursor;
            state.streams->Load(true, meshes, size_t(count) * 96);
            auto& original = state.pendingLegacyWorld["light_meshes"];
            original = Json{{"stride", 96}, {"vertex_stride", 32}, {"headers", HexBytes(meshes, size_t(count) * 96)},
                            {"vertices", Json::array()}};
            auto indexOffsets = static_cast<uint32_t*>(Retained(count * 4));
            auto vertexOffsets = static_cast<uint32_t*>(Retained(count * 4));
            auto indexCounts = static_cast<uint16_t*>(Retained(count * 2));
            auto vertexCounts = static_cast<uint16_t*>(Retained(count * 2));
            std::vector<uint16_t> indices;
            std::vector<uint8_t> vertices;
            for (uint32_t i = 0; i < count; ++i) {
                auto mesh = meshes + size_t(i) * 96;
                uint32_t ni{}, nv{}; uint64_t pi{}, pv{};
                std::memcpy(&ni, mesh, 4); std::memcpy(&pi, mesh + 8, 8);
                std::memcpy(&nv, mesh + 16, 4); std::memcpy(&pv, mesh + 24, 8);
                if (ni > UINT16_MAX || nv > UINT16_MAX || (ni && !pi) || (nv && !pv))
                    throw std::runtime_error("Invalid FDA light mesh extent");
                indexOffsets[i] = indices.size(); vertexOffsets[i] = vertices.size() / 16;
                indexCounts[i] = ni; vertexCounts[i] = nv;
                if (pi) {
                    state.streams->Align(1);
                    auto source = *state.cursor;
                    state.streams->Load(true, source, size_t(ni) * 2);
                    auto previous = indices.size(); indices.resize(previous + ni);
                    std::memcpy(indices.data() + previous, source, size_t(ni) * 2);
                }
                if (pv) {
                    state.streams->Align(3);
                    auto source = *state.cursor;
                    state.streams->Load(true, source, size_t(nv) * 32);
                    original["vertices"].push_back(Json{{"light", i}, {"bytes", HexBytes(source, size_t(nv) * 32)}});
                    for (size_t v = 0; v < nv; ++v)
                        vertices.insert(vertices.end(), source + v * 32, source + v * 32 + 16);
                }
            }
            auto pi = Retained(indices.size() * 2); auto pv = Retained(vertices.size());
            if (!indices.empty()) std::memcpy(pi, indices.data(), indices.size() * 2);
            if (!vertices.empty()) std::memcpy(pv, vertices.data(), vertices.size());
            uint32_t ni = indices.size(), nv = vertices.size() / 16;
            std::memcpy(output, &count, 4); std::memcpy(output + 4, &ni, 4); std::memcpy(output + 8, &nv, 4);
            for (auto [offset, pointer] : std::initializer_list<std::pair<size_t, void*>>{
                {16,indexOffsets},{24,vertexOffsets},{32,indexCounts},{40,vertexCounts},{48,pi},{56,pv}})
                std::memcpy(output + offset, &pointer, 8);
        }

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
            if constexpr (binding.type == 31) {
                if (state.opt->replayFileVersion == 0xfda)
                    state.serializedLayouts.emplace(copy, std::move(state.pendingLegacyWorld));
            }
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
        void LegacyString(char** slot) {
            auto value = reinterpret_cast<uintptr_t>(*slot);
            if (!value) return;
            if (value != UINT64_MAX && value != UINT64_MAX - 1) {
                *slot = reinterpret_cast<char*>(state.streams->Resolve(value, false));
                return;
            }
            if (value == UINT64_MAX) state.streams->SharedPush();
            *slot = reinterpret_cast<char*>(*state.cursor);
            state.streams->String(slot);
            if (value == UINT64_MAX) state.streams->SharedPop();
        }
        void LegacyComWorld(void** header) {
            auto value = reinterpret_cast<uintptr_t>(*header);
            state.streams->Push(1);
            if (value && value != UINT64_MAX && value != UINT64_MAX - 1 && value != UINT64_MAX - 2) {
                *header = state.streams->Resolve(value, true);
            } else if (value) {
                if (value == UINT64_MAX) state.streams->SharedPush();
                state.streams->Align(7);
                auto root = *state.cursor;
                *header = root;
                void** inserted = value == UINT64_MAX - 2 ? state.streams->Insert() : nullptr;
                state.streams->BeginAsset();
                state.streams->Load(true, root, 152);
                state.streams->Push(5);
                LegacyString(reinterpret_cast<char**>(root));
                uint32_t count{}; std::memcpy(&count, root + 48, 4);
                uint64_t pointer{}; std::memcpy(&pointer, root + 56, 8);
                if (count > 1000000) throw std::runtime_error("Legacy light count exceeds limit");
                if (pointer) {
                    state.streams->Align(7);
                    auto lights = *state.cursor;
                    std::memcpy(root + 56, &lights, 8);
                    state.streams->Load(true, lights, size_t(count) * 344);
                    for (size_t i = 0; i < count; ++i) LegacyString(reinterpret_cast<char**>(lights + i * 344 + 336));
                } else if (count) throw std::runtime_error("Legacy light array has null storage");
                LegacyString(reinterpret_cast<char**>(root + 80));
                std::memcpy(&count, root + 88, 4); std::memcpy(&pointer, root + 96, 8);
                if (count > 384) throw std::runtime_error("Legacy umbra gate count exceeds bit array");
                if (pointer) {
                    state.streams->Align(7);
                    auto names = reinterpret_cast<char**>(*state.cursor);
                    std::memcpy(root + 96, &names, 8);
                    state.streams->Load(true, names, size_t(count) * 8);
                    for (size_t i = 0; i < count; ++i) LegacyString(names + i);
                } else if (count) throw std::runtime_error("Legacy gate names have null storage");
                state.streams->Pop(); state.streams->EndAsset();
                if (state.ownedBytes > 128 * 1024 * 1024 - 168 || state.records.size() >= 1000000)
                    throw std::runtime_error("Legacy asset header budget exceeded");
                auto copy = state.ctx->zoneMemory.AllocAligned<byte>(168, 16);
                std::memset(copy, 0, 168); std::memcpy(copy, root, 152);
                state.owned.emplace_back(reinterpret_cast<uintptr_t>(copy), 168);
                state.ownedBytes += 168; state.records.push_back({24, copy}); *header = copy;
                if (inserted) *inserted = copy;
                if (value == UINT64_MAX) state.streams->SharedPop();
            }
            state.streams->Pop();
        }
        void NativeLoad(uint32_t type, void** header) {
            if (type == 24 && state.opt->replayFileVersion == 0xfda) {
                LegacyComWorld(header); return;
            }
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
                        const std::string rootType = record.type == 24 && state.opt->replayFileVersion == 0xfda
                            ? "ReplayFDA::ComWorld" : pool.at("root_type").get<std::string>();
                        auto inspection = inspector.Inspect(rootType, reinterpret_cast<uintptr_t>(record.header));
                        auto original = state.serializedLayouts.find(record.header);
                        if (original != state.serializedLayouts.end()) inspection["serialized_layout"] = original->second;
                        item["field_issues"] = inspection.at("issues");
                        item["field_nodes"] = inspection.at("nodes");
                        item["field_bytes_read"] = inspection.at("bytes_read");
                        for (auto key : { "read_errors",
                                          "unresolved_pointers",
                                          "unresolved_unions",
                                          "external_payloads",
                                          "runtime_references" })
                            item[key] = inspection.at(key);
                        auto outputPool = pool;
                        outputPool["root_type"] = rootType;
                        payload = ExportStructuredAsset(profile, outputPool, std::move(inspection), limits.maxBytes);
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
                std::memcpy(state.frustumLoaderPrologue.data(), mod[size_t(0xd91900)], state.frustumLoaderPrologue.size());
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
                mod.Redirect(size_t(0x19387d0), RetainImagePixels);
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
                state.lastAssetRoot = nullptr;
                state.legacyFrustumMeshes = 0;
                state.legacyViewStart = 0; state.legacyViewBytes = 0;
                state.serializedLayouts.clear(); state.pendingLegacyWorld = nullptr;
                hook::process::WriteMemSafe((*state.module)[size_t(0xd91900)], state.frustumLoaderPrologue.data(), state.frustumLoaderPrologue.size());
                if (opt.replayFileVersion == 0xfda) state.module->Redirect(size_t(0xd91900), LegacyFrustumLights);
                state.records.clear();
                state.owned.clear();
                state.ownedBytes = 0;
                state.retainedPixelBytes = 0;
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
                    state.report["xfile_version"] = opt.replayFileVersion;
                    state.report["input_xfile_version"] = opt.replayInputVersion;
                    state.report["applied_patches"] = opt.replayAppliedPatches;
                    if (ctx.blocksCount != 11 || !FastFileOption::IsReplayFileVersion(opt.replayFileVersion))
                        throw std::runtime_error("Expected a supported Replay revision with eleven header reservations");
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
                        state.report["loading_type_id"] = asset.type;
                        state.report["loading_serialized_offset"] = state.streams->Consumed();
                        NativeLoad(asset.type, &asset.header);
                        state.report["previous_type_id"] = asset.type;
                        state.report["previous_serialized_offset"] = state.report["loading_serialized_offset"];
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
                    // Each Link record has completed its native load and owns
                    // a retained root. Preserve those readable assets if a later
                    // asset fails, keeping the zone explicitly incomplete.
                    if (!state.report.at("complete").get<bool>() && !state.records.empty()) {
                        state.report["partial_load"] = true;
                        std::sort(state.owned.begin(), state.owned.end());
                        for (size_t i = 0; i < state.records.size(); ++i) {
                            ExportRecord(state.records[i]);
                            state.report["export_record_index"] = i + 1;
                            if (!((i + 1) % 128)) state.Progress();
                        }
                    }
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
