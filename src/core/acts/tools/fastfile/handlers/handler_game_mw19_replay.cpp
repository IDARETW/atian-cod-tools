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
#include <atomic>
#include <chrono>
#include <list>
#include <optional>
#include <thread>

namespace fastfile::handlers::mw19_replay {
    namespace {
        using namespace tool::mw19;
        using namespace tool::mw19::schema;
        using Clock = std::chrono::steady_clock;
        double MsSince(Clock::time_point start) {
            return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        }
        constexpr size_t kPoolCount = 117;
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
        using Range = std::pair<uintptr_t, size_t>;
        // Schema facts of one asset pool, read once: loading and export visit every asset of a zone.
        struct PoolInfo {
            std::string name;
            size_t size{}, nameOffset{};
            bool known{}, hasNameOffset{}, payloadExporter{}, selected{ true };
        };
        // Everything an export reads from one loaded fastfile. The memory behind it belongs to the load context
        // (one-shot runs) or to its serve cache entry.
        struct ZoneData {
            std::array<Range, 8> blocks{};
            std::vector<Range> owned;
            std::vector<Record> records;
            std::map<void*, Json> serializedLayouts;
            AssetList list{};
            uint32_t fileVersion{};
            Json loadReport = Json::object();
        };
        // A zone kept loaded by --serve.
        struct CachedZone {
            std::string key, name, file;
            ZoneData data;
            core::memory_allocator::MemoryAllocator memory;
            size_t bytes{};
        };
        struct State {
            hook::module_mapper::Module module{ true };
            std::unique_ptr<Database> database;
            std::unique_ptr<replay::Streams> streams;
            FastFileOption* opt{};
            FastFileContext* ctx{};
            uint8_t** cursor{};
            ZoneData scratch; // one-shot runs and loads in progress
            ZoneData* zone{ &scratch };
            size_t ownedBytes{};
            size_t retainedPixelBytes{};
            std::array<size_t, kPoolCount> sampled{};
            std::array<PoolInfo, kPoolCount> pools;
            std::array<const replay::Binding*, kPoolCount> bindingByType{};
            core::hashes::names::NamesStore names{ [](const char* name) { return hash::HashIWAsset(name); } };
            std::vector<xpak::Archive> packages;
            deps::oodle::Oodle oodle;
            xpak::OodleDecoder decode;
            std::filesystem::path out;
            Json report;
            std::ofstream journal;
            Clock::time_point journalFlushed{};
            std::ofstream streamTrace;
            bool traceOn{};
            size_t streamTraceEvents{};
            size_t current{};
            uint32_t currentType{};
            bool currentAssetActive{};
            uint32_t lastAssetType{};
            void* lastAssetRoot{};
            // Load position for manifest.json. Kept out of the JSON document while assets load: these change
            // for every asset and the manifest is only written now and then.
            struct LoadMark {
                bool set;
                uint32_t type;
                size_t bytes, offset;
            } lastLoad{}, loading{}, previous{};
            size_t exportIndex{};
            Clock::time_point lastProgress{};
            double initMs{}, initSchemaMs{}, initModuleMs{}, initPackagesMs{}, loadMs{}, exportMs{};
            uint64_t legacyFrustumMeshes{};
            std::array<uint8_t, 16> frustumLoaderPrologue{};
            uintptr_t legacyViewStart{};
            size_t legacyViewBytes{};
            Json pendingLegacyWorld;
            // ACTS_REPLAY_PRELOAD=1: load through Preload_XAssetHeader, the path Replay
            // uses for map zones, and record the postload stream plan it builds.
            bool preload{};
            int planDepth{};
            std::array<size_t, 8> planStart{};
            std::ofstream plan;
            size_t planEntries{}, planBlockZero{};
            bool planError{};
            // --serve: zones stay loaded between requests, most recently used first.
            bool serving{};
            size_t cacheBudget{};
            std::list<CachedZone> cache;
            std::string requestKey, requestTypes, requestNames;
            bool writeScriptStrings{ true };
            bool lastComplete{};
            std::string lastError;

            // Reads below touch this process's own committed allocations (the stream reservations and the retained
            // roots), so a bounds check and memcpy replace the ReadProcessMemory call used for foreign memory.
            bool Read(uint64_t address, void* destination, size_t size) const {
                bool valid = false;
                for (const auto& [start, length] : zone->blocks) {
                    if (address >= start && address - start <= length && size <= length - (address - start)) {
                        valid = true;
                        break;
                    }
                }
                if (!valid) {
                    // Sorted once loading finishes; export performs many
                    // field reads against these retained root allocations.
                    auto it = std::upper_bound(
                        zone->owned.begin(), zone->owned.end(), address,
                        [](uint64_t value, const Range& range) { return value < range.first; }
                    );
                    if (it != zone->owned.begin()) {
                        auto [start, length] = *--it;
                        valid = address - start <= length && size <= length - (address - start);
                    }
                }
                if (!valid)
                    return false;
                std::memcpy(destination, reinterpret_cast<const void*>(address), size);
                return true;
            }
            // Bytes readable from address inside one loader allocation (0 when it is outside all of them).
            size_t ReadableFrom(uint64_t address) const {
                for (const auto& [start, length] : zone->blocks)
                    if (address >= start && address - start < length)
                        return length - (address - start);
                auto it = std::upper_bound(
                    zone->owned.begin(), zone->owned.end(), address,
                    [](uint64_t value, const Range& range) { return value < range.first; }
                );
                if (it != zone->owned.begin()) {
                    auto [start, length] = *--it;
                    if (address - start < length)
                        return length - (address - start);
                }
                return 0;
            }
            // NUL-terminated string in loader memory, found with one scan. False when it is null, unterminated
            // within the limit or leaves its allocation: ReadString then reports the exact failure byte by byte.
            bool ReadCString(uint64_t address, std::string& value, size_t limit = 65536) const {
                if (!address)
                    return false;
                const size_t available = std::min(ReadableFrom(address), limit);
                if (!available)
                    return false;
                const auto begin = reinterpret_cast<const char*>(address);
                const auto end = static_cast<const char*>(std::memchr(begin, 0, available));
                if (!end)
                    return false;
                value.assign(begin, end);
                return true;
            }
            void SyncProgressFields() {
                if (streams) {
                    report["loaded_assets"] = zone->records.size();
                    report["serialized_bytes_read"] = streams->Consumed();
                    report["top_level_index"] = current;
                }
                if (loading.set) {
                    report["loading_type_id"] = loading.type;
                    report["loading_serialized_offset"] = loading.offset;
                }
                if (lastLoad.set)
                    report["last_asset_load"] =
                        Json{ { "type", lastLoad.type }, { "bytes", lastLoad.bytes }, { "offset", lastLoad.offset } };
                if (previous.set) {
                    report["previous_type_id"] = previous.type;
                    report["previous_serialized_offset"] = previous.offset;
                }
            }
            // manifest.json is the progress record of a run. Writing it replaces a file, which costs far more
            // than loading an asset, so the periodic calls (force = false) are limited to a few per second.
            void Progress(bool force = true) {
                if (out.empty())
                    return; // serve "load": nothing is written
                const auto now = Clock::now();
                if (!force && now - lastProgress < std::chrono::milliseconds(400))
                    return;
                lastProgress = now;
                SyncProgressFields();
                if (exportIndex)
                    report["export_record_index"] = exportIndex;
                WriteJsonAtomic(out / "manifest.json", report);
            }
        } state;
        Json StreamLocation(uint64_t address) {
            if (!state.streams) return nullptr;
            auto location = state.streams->Locate(address);
            if (!location) return nullptr;
            return Json{{"stream", location->first}, {"offset", location->second}};
        }
        Json StreamDelta(uint64_t address, uint64_t base) {
            if (!state.streams) return nullptr;
            auto location = state.streams->Locate(address);
            auto origin = state.streams->Locate(base);
            if (!location || !origin || location->first != origin->first || location->second < origin->second)
                return nullptr;
            return location->second - origin->second;
        }
// Loader hooks run millions of times per zone: the event is only built when a trace is being written.
#define REPLAY_TRACE(...)                                                                                              \
    do {                                                                                                               \
        if (state.traceOn)                                                                                             \
            TraceEvent(__VA_ARGS__);                                                                                   \
    } while (0)
        void TraceEvent(Json event) {
            if (!state.opt || !state.opt->replayStreamTrace || !state.streamTrace.is_open()) return;
            event["sequence"] = state.streamTraceEvents++;
            event["xasset_index"] = state.currentAssetActive ? Json(state.current) : Json(nullptr);
            event["type_id"] = state.currentAssetActive ? Json(state.currentType) : Json(nullptr);
            event["root_stream_location"] = StreamLocation(reinterpret_cast<uintptr_t>(state.lastAssetRoot));
            if (state.streams) {
                event["stream_block"] = state.streams->Block();
                event["stream_stack_depth"] = state.streams->Depth();
                event["serialized_input_cursor"] = state.streams->Consumed();
            }
            state.streamTrace << event.dump() << '\n';
        }
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
            auto callRva = reinterpret_cast<uintptr_t>(_ReturnAddress()) -
                           reinterpret_cast<uintptr_t>((*state.module)[size_t(0)]);
            REPLAY_TRACE(Json{{"kind", "load_stream_call"}, {"loader_start_arg", start},
                             {"at_start", start == 0},
                             {"size", size}, {"caller_rva", callRva},
                             {"destination_stream_location", StreamLocation(reinterpret_cast<uintptr_t>(destination))},
                             {"destination_root_delta", StreamDelta(reinterpret_cast<uintptr_t>(destination),
                                                                     reinterpret_cast<uintptr_t>(state.lastAssetRoot))}});
            if (start == 0 && destination == state.lastAssetRoot)
                state.lastLoad = { true, state.lastAssetType, size, state.streams->Consumed() };
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
        void PushStream(uint32_t block) {
            REPLAY_TRACE(Json{{"kind", "push_stream_call"}, {"requested_block", block}});
            state.streams->Push(block);
        }
        void PopStream() {
            REPLAY_TRACE(Json{{"kind", "pop_stream_call"}});
            state.streams->Pop();
        }
        void AlignStream(size_t mask) {
            REPLAY_TRACE(Json{{"kind", "align_stream_call"}, {"mask", mask}});
            state.streams->Align(mask);
        }
        void BeginAsset(int type, void* root) {
            state.lastAssetType = type; state.lastAssetRoot = root;
            REPLAY_TRACE(Json{{"kind", "begin_asset_call"}, {"asset_type", type},
                             {"root_stream_location", StreamLocation(reinterpret_cast<uintptr_t>(root))}});
            state.streams->BeginAsset();
        }
        void EndAsset() { REPLAY_TRACE(Json{{"kind", "end_asset_call"}}); state.streams->EndAsset(); }
        void SharedPush() { REPLAY_TRACE(Json{{"kind", "shared_push"}}); state.streams->SharedPush(); }
        void SharedPop() { REPLAY_TRACE(Json{{"kind", "shared_pop"}}); state.streams->SharedPop(); }
        void String(char** value) {
            REPLAY_TRACE(Json{{"kind", "string_call"},
                             {"field_stream_location", StreamLocation(reinterpret_cast<uintptr_t>(value))},
                             {"raw_pointer", reinterpret_cast<uintptr_t>(*value)}});
            state.streams->String(value);
        }
        void Resolve(uint64_t* value) {
            auto raw = *value;
            Json field = state.traceOn ? StreamLocation(reinterpret_cast<uintptr_t>(value)) : Json(nullptr);
            try {
                *value = reinterpret_cast<uintptr_t>(state.streams->Resolve(*value, false));
                REPLAY_TRACE(Json{{"kind", "packed_resolve"}, {"raw_token", raw},
                                 {"field_stream_location", field},
                                 {"resolved_target_stream_location", StreamLocation(*value)}});
            }
            catch (...) { state.report["failed_packed_pointer"] = *value; throw; }
        }
        void Alias(uint64_t* value) {
            auto raw = *value;
            Json field = state.traceOn ? StreamLocation(reinterpret_cast<uintptr_t>(value)) : Json(nullptr);
            try {
                *value = reinterpret_cast<uintptr_t>(state.streams->Resolve(*value, true));
                REPLAY_TRACE(Json{{"kind", "packed_alias"}, {"raw_token", raw},
                                 {"field_stream_location", field},
                                 {"resolved_target_stream_location", StreamLocation(*value)}});
            }
            catch (...) { state.report["failed_alias"] = *value; throw; }
        }
        void** Insert() {
            auto result = state.streams->Insert();
            REPLAY_TRACE(Json{{"kind", "insert_pointer_slot"},
                             {"inserted_stream_location", StreamLocation(reinterpret_cast<uintptr_t>(result))}});
            return result;
        }
        const void* Temporary(size_t length, size_t alignment) {
            REPLAY_TRACE(Json{{"kind", "temporary_payload_call"}, {"size", length}, {"alignment", alignment}});
            return state.streams->Temporary(length, alignment);
        }
        uintptr_t RuntimeOnly() { return 0; }
        // Native 0x11B2540 / 0x11B24B0 bracket every Preload_*Ptr. At depth zero the
        // closing call appends [block][int32 delta] per moved block 0..7 and a zero
        // terminator (0xF65F70); Postload replays one entry per pointer (0xF66050).
        void PlanBegin() {
            if (!state.planDepth++)
                state.planStart = state.streams->Offsets();
        }
        void PlanEnd() {
            // Called from native code: never throw here. An unbalanced bracket is reported after
            // CallNative returns (see the preload branch of the asset loader).
            if (state.planDepth <= 0) {
                state.planError = true;
                return;
            }
            if (--state.planDepth)
                return;
            auto now = state.streams->Offsets();
            Json deltas = Json::object();
            for (size_t block = 0; block < now.size(); ++block)
                if (now[block] != state.planStart[block]) {
                    deltas[std::to_string(block)] = int64_t(now[block]) - int64_t(state.planStart[block]);
                    if (!block) ++state.planBlockZero;
                }
            ++state.planEntries;
            if (state.plan.is_open())
                state.plan << Json{{"entry", state.planEntries - 1}, {"xasset_index", state.current},
                                   {"type_id", state.currentType},
                                   {"caller_rva", reinterpret_cast<uintptr_t>(_ReturnAddress()) -
                                                      reinterpret_cast<uintptr_t>((*state.module)[size_t(0)])},
                                   {"deltas", deltas}}.dump()
                           << '\n';
        }
        size_t RetainedPayloadBudget() {
            // Resident pixels are copied out of Replay's rewindable stream 1.
            // Bound those copies to the serialized input that supplied them,
            // plus a fixed allowance for normalized legacy records and other
            // small retained helper arrays. A fixed 512 MiB total rejected
            // otherwise valid image-heavy zones whose own serialized input is
            // larger than that value.
            constexpr size_t allowance = 512ull * 1024 * 1024;
            const size_t input = state.streams ? state.streams->InputSize() : 0;
            return input > SIZE_MAX - allowance ? SIZE_MAX : input + allowance;
        }
        void RetainImagePixels(uint8_t** pixels, const uint8_t* image) {
            // Load_GfxImagePixels uses rewindable stream 1. Capture at the
            // upload boundary, before DB_PopStreamPos reuses those bytes.
            uint32_t size{};
            if (!image || !state.streams->Contains(reinterpret_cast<uintptr_t>(image), 232))
                throw std::runtime_error("Image upload header outside loader memory");
            std::memcpy(&size, image + 28, 4);
            if (!pixels || !*pixels || !size) return;
            const size_t budget = RetainedPayloadBudget();
            if (state.retainedPixelBytes > budget || size > budget - state.retainedPixelBytes ||
                !state.streams->Contains(reinterpret_cast<uintptr_t>(*pixels), size))
                throw std::runtime_error("Resident image pixels exceed retained payload bounds");
            auto copy = state.ctx->zoneMemory.AllocAligned<uint8_t>(size, 16);
            std::memcpy(copy, *pixels, size);
            state.zone->owned.emplace_back(reinterpret_cast<uintptr_t>(copy), size);
            state.retainedPixelBytes += size;
            *pixels = copy;
        }
        void ScriptString(uint32_t*) {} // Preserve the fastfile's script-string indices.

        void* Retained(size_t bytes, size_t alignment) {
            const size_t budget = RetainedPayloadBudget();
            if (state.retainedPixelBytes > budget || bytes > budget - state.retainedPixelBytes)
                throw std::runtime_error("Retained loader payload budget exceeded");
            auto result = state.ctx->zoneMemory.AllocAligned<uint8_t>(std::max(size_t(1), bytes), alignment);
            std::memset(result, 0, bytes);
            state.zone->owned.emplace_back(reinterpret_cast<uintptr_t>(result), bytes);
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
            const size_t size = state.pools[binding.type].size;
            if (!header || !*header || !state.streams->Contains(reinterpret_cast<uintptr_t>(*header), size))
                throw std::runtime_error("Replay asset header outside loader memory");
            REPLAY_TRACE(Json{{"kind", "asset_root_link"}, {"asset_type", binding.type},
                             {"root_bytes", size},
                             {"root_stream_location", StreamLocation(reinterpret_cast<uintptr_t>(*header))},
                             {"root_archive_input_cursor", state.streams->Consumed()}});
            if (size > 128 * 1024 * 1024 - state.ownedBytes || state.zone->records.size() >= 1000000)
                throw std::runtime_error("Replay asset header budget exceeded");
            void* copy = state.ctx->zoneMemory.AllocAligned<byte>(size, 16);
            std::memcpy(copy, *header, size);
            state.zone->owned.emplace_back(reinterpret_cast<uintptr_t>(copy), size);
            state.ownedBytes += size;
            *header = copy;
            state.zone->records.push_back({ binding.type, copy });
            if constexpr (binding.type == 31) {
                if (state.opt->replayFileVersion == 0xfda)
                    state.zone->serializedLayouts.emplace(copy, std::move(state.pendingLegacyWorld));
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
                if (state.ownedBytes > 128 * 1024 * 1024 - 168 || state.zone->records.size() >= 1000000)
                    throw std::runtime_error("Legacy asset header budget exceeded");
                auto copy = state.ctx->zoneMemory.AllocAligned<byte>(168, 16);
                std::memset(copy, 0, 168); std::memcpy(copy, root, 152);
                state.zone->owned.emplace_back(reinterpret_cast<uintptr_t>(copy), 168);
                state.ownedBytes += 168; state.zone->records.push_back({24, copy}); *header = copy;
                if (inserted) *inserted = copy;
                if (value == UINT64_MAX) state.streams->SharedPop();
            }
            state.streams->Pop();
        }
        void NativeLoad(uint32_t type, void** header) {
            if (type == 24 && state.opt->replayFileVersion == 0xfda) {
                LegacyComWorld(header); return;
            }
            const replay::Binding* it = type < kPoolCount ? state.bindingByType[type] : nullptr;
            if (!it)
                throw std::runtime_error("Replay asset has no enabled loader: " + std::to_string(type));
            uintptr_t fault{};
            int code{};
            if (state.preload) {
                // Preload_XAssetHeader (RVA 0xDCBFD0; symbol map 0001:00DCAFD0) reads the XAsset* at
                // 0x5D414B0 and &header at 0x5D414D0, which its caller Preload_XAsset (RVA 0xDCBF80) sets.
                state.planError = false;
                *reinterpret_cast<void**>((*state.module)[0x5d414b0]) = reinterpret_cast<uint8_t*>(header) - 8;
                *reinterpret_cast<void***>((*state.module)[0x5d414d0]) = header;
                code = CallNative(reinterpret_cast<void (*)(int)>((*state.module)[0xdcbfd0]), 1, &fault);
                if (!code && state.planError)
                    throw std::runtime_error("Replay postload plan underflow (PlanEnd without PlanBegin)");
                if (!code && state.planDepth)
                    throw std::runtime_error("Replay postload plan left open after asset");
            }
            else {
                *reinterpret_cast<void***>((*state.module)[it->global]) = header;
                code = CallNative(reinterpret_cast<void (*)(int)>((*state.module)[it->loader]), 1, &fault);
            }
            if (code)
                throw std::runtime_error(
                    std::format("Replay native loader fault 0x{:x} at 0x{:x}, type {}", uint32_t(code), fault, type)
                );
        }

        void ExportRecord(const Record& record) {
            const PoolInfo& info = state.pools.at(record.type);
            if (!info.selected)
                return;
            if (state.opt->replayTest && state.sampled[record.type] >= state.opt->replayLimit)
                return;
            ++state.sampled[record.type];
            // A name filter skips most records: read the name before anything is built for the journal.
            std::string name;
            uint64_t nameAddress{};
            const bool named =
                info.hasNameOffset &&
                state.Read(reinterpret_cast<uintptr_t>(record.header) + info.nameOffset, &nameAddress, 8) &&
                state.ReadCString(nameAddress, name);
            if (named && !state.names.Contains(hash::HashIWAsset(name.c_str()), true)) {
                --state.sampled[record.type];
                return;
            }
            const auto& profile = state.database->Profile("replay-1.20");
            const auto& pool = profile.at("pools").at(record.type);
            const std::string& type = info.name;
            Json item{ { "type", type }, { "type_id", record.type }, { "status", "pending" } };
            MemoryReader read = [&](uint64_t a, void* b, size_t n) { return state.Read(a, b, n); };
            try {
                if (!named) {
                    // The checked path: it reports why the name cannot be read.
                    if (!read(
                            reinterpret_cast<uintptr_t>(record.header) + pool.at("name_offset").get<size_t>(),
                            &nameAddress,
                            8
                        ))
                        throw std::runtime_error("Cannot read asset name pointer");
                    name = ReadString(read, nameAddress);
                    if (!state.names.Contains(hash::HashIWAsset(name.c_str()), true)) {
                        --state.sampled[record.type];
                        return;
                    }
                }
                item["name"] = name;
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
                    // ACTS_REPLAY_STRUCTURED=1: export the structured asset (e.g. a sound
                    // bank's alias table) instead of its binary payload.
                    static const bool structuredOnly = [] {
                        const char* value = std::getenv("ACTS_REPLAY_STRUCTURED");
                        return value && *value == '1';
                    }();
                    if (info.payloadExporter && !structuredOnly) {
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
                        limits.maxBytes = structuredOnly ? size_t{ 2047 } * 1024 * 1024 : 256 * 1024 * 1024;
                        limits.maxNodes = structuredOnly ? 60000000 : 4000000;
                        limits.maxArray = 32000000;
                        limits.maxDepth = 64;
                        limits.compactArrays = true;
                        Inspector inspector(*state.database, read, limits, "replay-1.20");
                        const std::string rootType = record.type == 24 && state.zone->fileVersion == 0xfda
                            ? "ReplayFDA::ComWorld" : pool.at("root_type").get<std::string>();
                        auto inspection = inspector.Inspect(rootType, reinterpret_cast<uintptr_t>(record.header));
                        auto original = state.zone->serializedLayouts.find(record.header);
                        if (original != state.zone->serializedLayouts.end()) inspection["serialized_layout"] = original->second;
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
            // The journal is flushed a few times per second (and when the export ends), not once per asset.
            if (const auto now = Clock::now(); now - state.journalFlushed >= std::chrono::milliseconds(200)) {
                state.journalFlushed = now;
                state.journal.flush();
            }
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
                std::string value;
                if (!state.ReadCString(reinterpret_cast<uintptr_t>(list.strings[i]), value))
                    value = ReadString(read, reinterpret_cast<uintptr_t>(list.strings[i]));
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
            if (!state.opt->replayTest && state.writeScriptStrings) {
                WriteJsonAtomic(state.out / "script_strings.json", strings);
                state.report["script_strings_file"] = "script_strings.json";
            }
        }
        // Stream reservations run from megabytes to gigabytes. They are reserved and committed here and
        // zero-filled by the system on first touch, so nothing is cleared (or made resident) up front.
        // Streams::Align pads from the absolute cursor: every base is aligned to the largest stream alignment.
        uint8_t* AllocateBlock(core::memory_allocator::MemoryAllocator& memory, size_t size) {
            constexpr size_t alignment = 0x100000, slack = 0x10000;
            const size_t bytes = std::max<size_t>(size, 1) + slack;
            void* base = VirtualAlloc(nullptr, bytes + alignment, MEM_RESERVE, PAGE_NOACCESS);
            if (!base)
                throw std::runtime_error("Cannot reserve Replay stream memory");
            memory.AddCustom(base, [](void* ptr) { VirtualFree(ptr, 0, MEM_RELEASE); }, bytes);
            auto aligned =
                reinterpret_cast<uint8_t*>((reinterpret_cast<uintptr_t>(base) + alignment - 1) & ~(alignment - 1));
            if (!VirtualAlloc(aligned, bytes, MEM_COMMIT, PAGE_READWRITE))
                throw std::runtime_error("Cannot commit Replay stream memory");
            return aligned;
        }

        double Tenths(double ms) { return std::round(ms * 10) / 10; }

        // Identity of a fastfile on disk (with its delta patches) for the serve cache.
        std::string ZoneKey(const std::filesystem::path& file) {
            std::error_code ec;
            auto full = std::filesystem::weakly_canonical(file, ec);
            if (ec)
                full = std::filesystem::absolute(file, ec);
            std::string key = full.generic_string();
            std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            for (const char* extension : { ".ff", ".fp", ".fc" }) {
                auto part = full;
                part.replace_extension(extension);
                const auto size = std::filesystem::file_size(part, ec);
                const auto time = ec ? std::filesystem::file_time_type{} : std::filesystem::last_write_time(part, ec);
                key += std::format("|{}:{}", ec ? 0 : size, ec ? 0 : time.time_since_epoch().count());
            }
            return key;
        }

        // -a / -n (or the fields of a serve request) to the per-pool selection and the name filter.
        void ConfigureSelection(FastFileOption& opt) {
            state.names.LoadConfig(opt.assets);
            std::set<std::string> selected;
            if (opt.assetTypes) {
                std::string selection{ opt.assetTypes };
                std::replace(selection.begin(), selection.end(), ',', ' ');
                std::istringstream tokens(selection);
                for (std::string type; tokens >> type;) {
                    state.database->Pool(state.database->Profile("replay-1.20"), type);
                    selected.insert(type);
                }
            }
            for (PoolInfo& info : state.pools)
                info.selected = selected.empty() || selected.contains(info.name);
        }

        // Output folder, manifest skeleton and journal of one export. An empty output path (serve "load")
        // writes nothing.
        void BeginOutput(FastFileOption& opt, const std::string& name, const std::string& file) {
            state.streamTrace.close();
            state.streamTrace.clear();
            state.streamTraceEvents = 0;
            state.traceOn = false;
            state.planDepth = 0;
            state.planEntries = state.planBlockZero = 0;
            state.plan.close();
            state.plan.clear();
            state.journal.close();
            state.journal.clear();
            state.exportIndex = 0;
            state.lastProgress = {};
            state.journalFlushed = Clock::now();
            state.report =
                Json{ { "profile", "replay-1.20" }, { "fastfile", file },            { "complete", false },
                      { "success", false },         { "test_only", opt.replayTest }, { "tested", size_t(0) },
                      { "failed", size_t(0) },      { "unavailable", size_t(0) },    { "references", size_t(0) },
                      { "journal", "assets.jsonl" } };
            if (opt.m_output.empty()) {
                state.out.clear();
                return;
            }
            state.out = opt.m_output / "mw19replay" / name;
            std::filesystem::create_directories(state.out);
            if (opt.replayStreamTrace) {
                state.streamTrace.open(state.out / "stream-trace.jsonl", std::ios::binary | std::ios::trunc);
                if (!state.streamTrace) throw std::runtime_error("Cannot create Replay stream trace");
                state.traceOn = true;
            }
            if (state.preload) {
                state.plan.open(state.out / "preload-plan.jsonl", std::ios::binary | std::ios::trunc);
                state.report["preload_plan"] = "preload-plan.jsonl";
            }
            state.journal.open(state.out / "assets.jsonl", std::ios::binary | std::ios::trunc);
            if (!state.journal)
                throw std::runtime_error("Cannot create Replay asset report");
        }

        void CloseTrace() {
            if (state.streamTrace.is_open()) {
                state.streamTrace.flush();
                state.streamTrace.close();
                state.report["stream_trace_events"] = state.streamTraceEvents;
            }
            state.traceOn = false;
        }

        // Runs the native loaders over the decompressed stream. Throws on the first asset that cannot be loaded;
        // the records linked until then stay readable.
        void LoadZone(FastFileOption& opt, core::bytebuffer::ByteBuffer& reader, FastFileContext& ctx) {
            if (ctx.blocksCount != 11 || !FastFileOption::IsReplayFileVersion(opt.replayFileVersion))
                throw std::runtime_error("Expected a supported Replay revision with eleven header reservations");
            std::array<std::span<uint8_t>, 8> blocks;
            size_t total{};
            for (size_t i = 0; i < blocks.size(); ++i) {
                size_t size = ctx.blockSizes[i].size;
                if (size > 2ull * 1024 * 1024 * 1024 - total)
                    throw std::runtime_error("Replay stream reservations exceed 2 GiB");
                total += size;
                auto data = AllocateBlock(ctx.zoneMemory, size);
                blocks[i] = { data, size };
                state.zone->blocks[i] = { reinterpret_cast<uintptr_t>(data), size };
            }
            state.streams = std::make_unique<replay::Streams>(
                blocks,
                std::span<const uint8_t>(reader.Ptr<byte>(), reader.Remaining()),
                *state.cursor
            );
            if (opt.replayStreamTrace) {
                state.streams->SetTrace([](const replay::StreamTraceEvent& event) {
                    TraceEvent(Json{{"kind", "stream_operation"}, {"operation", event.operation},
                                    {"block_before", event.blockBefore},
                                    {"offset_before", event.offsetBefore},
                                    {"input_before", event.inputBefore},
                                    {"block_after", event.blockAfter},
                                    {"offset_after", event.offsetAfter},
                                    {"input_after", event.inputAfter},
                                    {"value", event.value}, {"size", event.size},
                                    {"physical_input_bytes", event.inputAfter - event.inputBefore}});
                });
            }
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
            state.zone->list = list;
            for (; state.current < list.count; ++state.current) {
                auto& asset = list.assets[state.current];
                state.currentType = asset.type;
                state.currentAssetActive = true;
                state.loading = { true, asset.type, 0, state.streams->Consumed() };
                REPLAY_TRACE(Json{{"kind", "xasset_begin"}, {"xasset_index", state.current},
                                 {"asset_type", asset.type}, {"pointer_token", reinterpret_cast<uintptr_t>(asset.header)},
                                 {"table_entry_stream_location", StreamLocation(reinterpret_cast<uintptr_t>(&asset))},
                                 {"serialized_input_cursor", state.streams->Consumed()}});
                NativeLoad(asset.type, &asset.header);
                REPLAY_TRACE(Json{{"kind", "xasset_end"}, {"xasset_index", state.current},
                                 {"asset_type", asset.type},
                                 {"resolved_header_stream_location", StreamLocation(reinterpret_cast<uintptr_t>(asset.header))},
                                 {"serialized_input_cursor", state.streams->Consumed()}});
                state.currentAssetActive = false;
                state.previous = { true, asset.type, 0, state.loading.offset };
                if (!(state.current % 256))
                    state.Progress(false);
            }
            state.streams->Pop();
            state.streams->Finish();
            if (state.preload) {
                state.report["preload_plan_entries"] = state.planEntries;
                state.report["preload_plan_block0_entries"] = state.planBlockZero;
                state.plan.close();
            }
            std::sort(state.zone->owned.begin(), state.zone->owned.end());
        }

        // What manifest.json says about the load itself, kept with the zone so that an export from the serve
        // cache reports the same fields as the run that loaded it.
        void SaveLoadReport() {
            state.SyncProgressFields();
            size_t reserved{}, used{};
            for (size_t bytes : state.streams->Reserved())
                reserved += bytes;
            for (size_t bytes : state.streams->Peaks())
                used += bytes;
            state.report["stream_bytes"] = Json{ { "reserved", reserved },
                                                 { "used", used },
                                                 { "retained", state.ownedBytes + state.retainedPixelBytes } };
            for (const char* key :
                 { "xfile_version", "input_xfile_version", "applied_patches", "loaded_assets", "serialized_bytes_read",
                   "top_level_index", "loading_type_id", "loading_serialized_offset", "last_asset_load",
                   "previous_type_id", "previous_serialized_offset", "preload_plan_entries",
                   "preload_plan_block0_entries", "stream_bytes" })
                if (state.report.contains(key))
                    state.zone->loadReport[key] = state.report.at(key);
        }

        // --serve: the cache takes the zone and its memory; the least recently used zones beyond the budget go.
        void Retain(FastFileContext& ctx) {
            CachedZone entry;
            entry.key = state.requestKey.empty() ? ZoneKey(ctx.file) : state.requestKey;
            entry.name = ctx.ffname;
            entry.file = ctx.file;
            entry.data = std::move(state.scratch);
            state.scratch = ZoneData{};
            entry.memory.Adopt(ctx.zoneMemory);
            entry.bytes = state.ownedBytes + state.retainedPixelBytes;
            for (size_t used : state.streams->Peaks())
                entry.bytes += used;
            state.cache.remove_if([&](const CachedZone& zone) { return zone.key == entry.key; });
            state.cache.push_front(std::move(entry));
            state.zone = &state.cache.front().data;
            size_t total{};
            for (const CachedZone& zone : state.cache)
                total += zone.bytes;
            while (state.cache.size() > 1 && total > state.cacheBudget) {
                const CachedZone& oldest = state.cache.back();
                total -= oldest.bytes;
                LOG_INFO("Replay serve: releasing {} ({} MiB)", oldest.name, oldest.bytes >> 20);
                state.cache.pop_back();
            }
        }

        // Script strings, coverage and the selected records of the current zone; closes the journal and the
        // manifest. Throws when an asset failed or a payload was unavailable, after everything is written.
        void ExportZone(FastFileOption& opt) {
            const auto start = Clock::now();
            state.sampled.fill(0);
            state.exportIndex = 0;
            ExportScriptStrings(state.zone->list);
            {
                // Keys in order of first appearance, as a per-record insert would give.
                std::array<size_t, kPoolCount> counts{};
                std::vector<uint32_t> order;
                for (const auto& record : state.zone->records)
                    if (!counts.at(record.type)++)
                        order.push_back(record.type);
                auto& coverage = state.report["loaded_by_type"] = Json::object();
                for (uint32_t type : order)
                    coverage[state.pools[type].name] = counts[type];
            }
            state.Progress();
            const auto& records = state.zone->records;
            for (size_t i = 0; i < records.size(); ++i) {
                ExportRecord(records[i]);
                state.exportIndex = i + 1;
                if (!((i + 1) % 128))
                    state.Progress(false);
            }
            state.report["complete"] = true;
            state.report["success"] =
                !state.report.at("failed").get<size_t>() && !state.report.at("unavailable").get<size_t>();
            state.journal.flush();
            if (!state.journal)
                throw std::runtime_error("Cannot write Replay asset report");
            state.journal.close();
            CloseTrace();
            state.exportMs = MsSince(start);
            const bool cached = !state.streams;
            Json timing{ { "init", Tenths(state.initMs) },
                         { "init_schema", Tenths(state.initSchemaMs) },
                         { "init_module", Tenths(state.initModuleMs) },
                         { "init_packages", Tenths(state.initPackagesMs) } };
            if (!cached) {
                timing["read"] = Tenths(opt.perfReadMs);
                timing["decompress"] = Tenths(opt.perfDecompressMs);
                timing["patch"] = Tenths(opt.perfPatchMs);
                timing["load"] = Tenths(state.loadMs);
            }
            timing["export"] = Tenths(state.exportMs);
            state.report["timing_ms"] = std::move(timing);
            if (state.serving)
                state.report["cached"] = cached;
            state.lastComplete = true;
            state.Progress();
            if (state.preload)
                LOG_INFO("Replay preload: {} postload plan entries, {} move block 0", state.planEntries,
                         state.planBlockZero);
            LOG_INFO(
                "Replay: {} assets loaded, {} tested, {} failed, {} unavailable",
                state.zone->records.size(),
                state.report.at("tested").get<size_t>(),
                state.report.at("failed").get<size_t>(),
                state.report.at("unavailable").get<size_t>()
            );
            if (state.report.at("failed").get<size_t>() || state.report.at("unavailable").get<size_t>())
                throw std::runtime_error(
                    "Replay asset export has failures or unavailable payloads; see manifest.json"
                );
        }

        void Respond(const Json& message) {
            std::cout << "@acts " << message.dump(-1, ' ', false, Json::error_handler_t::replace) << std::endl;
        }


        class Handler : public FFHandler {
          public:
            Handler()
                : FFHandler(
                      "mw19replay", "Modern Warfare 2019 1.20 Replay",
                      compatibility::scobalula::csi::CordycepGame::CG_MW4, true
                  ) {}
            void Init(FastFileOption& opt) override {
                const auto initStart = Clock::now();
                state.opt = &opt;
                state.packages.clear();
                state.decode = {};
                state.database = std::make_unique<Database>(utils::GetProgDir() / "data/mw19/schema.json");
                state.initSchemaMs = MsSince(initStart);
                if (!opt.game)
                    throw std::runtime_error("mw19replay requires -g with the 1.20 Replay executable");
                const auto moduleStart = Clock::now();
                if (!state.module.Load(opt.game, false))
                    throw std::runtime_error("Cannot map Replay executable");
                state.initModuleMs = MsSince(moduleStart);
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
                {
                    const auto& pools = state.database->Profile("replay-1.20").at("pools");
                    if (pools.size() > kPoolCount)
                        throw std::runtime_error("Replay profile has more pools than the handler supports");
                    state.pools = {};
                    for (size_t i = 0; i < pools.size(); ++i) {
                        const auto& pool = pools[i];
                        PoolInfo& info = state.pools[i];
                        info.known = true;
                        info.name = pool.at("name").get<std::string>();
                        info.size = pool.at("size").get<size_t>();
                        if (pool.contains("name_offset") && pool.at("name_offset").is_number_unsigned()) {
                            info.nameOffset = pool.at("name_offset").get<size_t>();
                            info.hasNameOffset = true;
                        }
                        info.payloadExporter = HasPayloadExporter(info.name);
                    }
                    state.bindingByType.fill(nullptr);
                    for (const auto& binding : replay::bindings) {
                        if (binding.type >= kPoolCount || !state.pools[binding.type].known)
                            throw std::runtime_error("Replay loader binding names an unknown pool");
                        if (!state.bindingByType[binding.type])
                            state.bindingByType[binding.type] = &binding;
                    }
                }
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
                const char* preloadValue = std::getenv("ACTS_REPLAY_PRELOAD");
                state.preload = preloadValue && preloadValue[0] == '1' && !preloadValue[1];
                if (state.preload) {
                    mod.Redirect(size_t(0x11b2540), PlanBegin);
                    mod.Redirect(size_t(0x11b24b0), PlanEnd);
                }
                if (state.preload && opt.replayServe)
                    throw std::runtime_error("--serve cannot be combined with ACTS_REPLAY_PRELOAD");
                InstallLinks(std::make_index_sequence<std::size(replay::bindings)>{});
                ConfigureSelection(opt);
                const auto packagesStart = Clock::now();
                {
                    // Each archive index (up to a few hundred thousand entries) is read and checked on its own
                    // thread; errors and the budget are then applied in command-line order.
                    const size_t count = opt.replayPackages.size();
                    std::vector<std::optional<xpak::Archive>> opened(count);
                    std::vector<std::exception_ptr> errors(count);
                    std::atomic<size_t> next{ 0 };
                    auto open = [&] {
                        for (size_t i; (i = next.fetch_add(1)) < count;) {
                            try {
                                opened[i].emplace(xpak::Archive::Open(opt.replayPackages[i]));
                            } catch (...) {
                                errors[i] = std::current_exception();
                            }
                        }
                    };
                    {
                        std::vector<std::thread> pool;
                        for (size_t t = 1; t < std::min<size_t>(count, 8); ++t)
                            pool.emplace_back(open);
                        open();
                        for (std::thread& t : pool)
                            t.join();
                    }
                    size_t indexBytes{};
                    for (size_t i = 0; i < count; ++i) {
                        if (errors[i])
                            std::rethrow_exception(errors[i]);
                        indexBytes += opened[i]->Entries().size() * sizeof(xpak::Entry);
                        if (indexBytes > 128 * 1024 * 1024)
                            throw std::runtime_error("Replay XPak index budget exceeded");
                        state.packages.push_back(std::move(*opened[i]));
                    }
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
                state.initPackagesMs = MsSince(packagesStart);
                state.serving = opt.replayServe;
                state.cacheBudget = opt.replayServeCacheMb * 1024 * 1024;
                state.writeScriptStrings = true;
                state.initMs = MsSince(initStart);
            }
            void Cleanup() override {
                state.streams.reset();
                state.zone = &state.scratch;
                state.scratch = ZoneData{};
                state.cache.clear();
                state.packages.clear();
                state.journal.close();
                state.streamTrace.close();
                state.database.reset();
                state.module.Free();
            }
            void Handle(FastFileOption& opt, core::bytebuffer::ByteBuffer& reader, FastFileContext& ctx) override {
                state.streams.reset();
                state.ctx = &ctx;
                state.zone = &state.scratch;
                state.scratch = ZoneData{};
                state.lastAssetRoot = nullptr;
                state.legacyFrustumMeshes = 0;
                state.legacyViewStart = 0; state.legacyViewBytes = 0;
                state.pendingLegacyWorld = nullptr;
                hook::process::WriteMemSafe((*state.module)[size_t(0xd91900)], state.frustumLoaderPrologue.data(), state.frustumLoaderPrologue.size());
                if (opt.replayFileVersion == 0xfda) state.module->Redirect(size_t(0xd91900), LegacyFrustumLights);
                state.ownedBytes = 0;
                state.retainedPixelBytes = 0;
                state.sampled.fill(0);
                state.current = 0;
                state.currentType = 0;
                state.currentAssetActive = false;
                state.lastLoad = state.loading = state.previous = {};
                state.loadMs = state.exportMs = 0;
                state.lastComplete = false;
                state.lastError.clear();
                state.zone->fileVersion = opt.replayFileVersion;
                BeginOutput(opt, ctx.ffname, ctx.file);
                try {
                    state.report["xfile_version"] = opt.replayFileVersion;
                    state.report["input_xfile_version"] = opt.replayInputVersion;
                    state.report["applied_patches"] = opt.replayAppliedPatches;
                    if (opt.replayStreamTrace) state.report["stream_trace"] = "stream-trace.jsonl";
                    const auto loadStart = Clock::now();
                    LoadZone(opt, reader, ctx);
                    state.loadMs = MsSince(loadStart);
                    SaveLoadReport();
                    if (state.serving)
                        Retain(ctx);
                    if (state.out.empty()) {
                        // Serve "load": the zone is in memory, nothing is exported.
                        LOG_INFO("Replay: {} assets loaded", state.zone->records.size());
                        state.lastComplete = true;
                    } else {
                        ExportZone(opt);
                    }
                } catch (const std::exception& error) {
                    state.lastError = error.what();
                    state.report["error"] = error.what();
                    CloseTrace();
                    // Each Link record has completed its native load and owns
                    // a retained root. Preserve those readable assets if a later
                    // asset fails, keeping the zone explicitly incomplete.
                    if (!state.out.empty() && !state.report.at("complete").get<bool>() &&
                        !state.zone->records.empty()) {
                        state.report["partial_load"] = true;
                        std::sort(state.zone->owned.begin(), state.zone->owned.end());
                        for (size_t i = 0; i < state.zone->records.size(); ++i) {
                            ExportRecord(state.zone->records[i]);
                            state.exportIndex = i + 1;
                            if (!((i + 1) % 128)) state.Progress(false);
                        }
                    }
                    state.Progress();
                    state.journal.close();
                    state.streams.reset();
                    if (state.zone == &state.scratch)
                        state.scratch = ZoneData{}; // its memory goes with the load context
                    throw;
                }
                state.streams.reset();
                if (state.zone == &state.scratch)
                    state.scratch = ZoneData{};
            }

            // --serve. One JSON request per line on stdin, one "@acts {json}" answer per request on stdout
            // (docs/mw19/fastfile.md, "Serve mode"). Any other output line is ordinary log text.
            bool Serve(FastFileOption& opt, const std::function<bool(const char* file)>& load) override {
                const auto setRequest = [&](const Json& request) {
                    state.requestTypes = request.value("types", std::string{});
                    state.requestNames.clear();
                    if (request.contains("names")) {
                        const Json& names = request.at("names");
                        if (names.is_string())
                            state.requestNames = names.get<std::string>();
                        else
                            for (const auto& name : names) {
                                const auto text = name.get<std::string>();
                                if (text.empty() || text.find(',') != std::string::npos)
                                    throw std::runtime_error("Invalid asset name in request");
                                if (!state.requestNames.empty())
                                    state.requestNames += ',';
                                state.requestNames += text;
                            }
                    }
                    opt.assetTypes = state.requestTypes.empty() ? nullptr : state.requestTypes.c_str();
                    opt.assets = state.requestNames.empty() ? nullptr : state.requestNames.c_str();
                    opt.replayGeometry = request.value("geometry", false);
                    opt.noAssetDump = request.value("inventory", false);
                    opt.replayTest = request.value("test", false);
                    opt.replayLimit = request.value("limit", size_t{ 1 });
                    if (!opt.replayLimit || opt.replayLimit > 32 || (opt.replayGeometry && opt.noAssetDump))
                        throw std::runtime_error("Invalid request options");
                    state.writeScriptStrings = request.value("script_strings", false);
                    ConfigureSelection(opt);
                };
                const auto summary = [&](Json& response) {
                    for (const char* key : { "tested", "failed", "unavailable", "references" })
                        response[key] = state.report.value(key, size_t(0));
                    response["loaded"] = state.zone->records.size();
                    if (!state.out.empty())
                        response["dir"] = state.out.generic_string();
                    // What is in memory now, most recently used first: a client with several servers sends a
                    // request to the one that already holds its zone.
                    Json zones = Json::array();
                    for (const CachedZone& zone : state.cache)
                        zones.push_back(zone.name);
                    response["zones"] = std::move(zones);
                };
                Respond(Json{ { "ready", true },
                              { "protocol", 1 },
                              { "pid", static_cast<uint64_t>(GetCurrentProcessId()) },
                              { "cache_mb", opt.replayServeCacheMb },
                              { "init_ms", Tenths(state.initMs) } });
                // Fastfiles named on the command line are loaded up front and kept; nothing is exported for them.
                for (const char* file : opt.files) {
                    const auto output = opt.m_output;
                    opt.m_output.clear();
                    state.requestKey = ZoneKey(file);
                    load(file);
                    opt.m_output = output;
                    state.requestKey.clear();
                    state.zone = &state.scratch;
                }
                std::string line;
                while (std::getline(std::cin, line)) {
                    while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                        line.pop_back();
                    if (line.empty())
                        continue;
                    const auto start = Clock::now();
                    Json response = Json::object();
                    bool quit = false;
                    try {
                        const Json request = Json::parse(line);
                        if (!request.is_object())
                            throw std::runtime_error("A request is one JSON object per line");
                        response["id"] = request.value("id", Json(nullptr));
                        const auto command = request.value("cmd", std::string{ "export" });
                        if (command == "quit") {
                            quit = true;
                            response["ok"] = true;
                        } else if (command == "ping") {
                            response["ok"] = true;
                        } else if (command == "trim") {
                            state.zone = &state.scratch;
                            state.cache.clear();
                            response["ok"] = true;
                        } else if (command == "stats") {
                            Json zones = Json::array();
                            size_t total{};
                            for (const CachedZone& zone : state.cache) {
                                total += zone.bytes;
                                zones.push_back(Json{ { "zone", zone.name },
                                                      { "file", zone.file },
                                                      { "bytes", zone.bytes },
                                                      { "assets", zone.data.records.size() } });
                            }
                            response["ok"] = true;
                            response["zones"] = std::move(zones);
                            response["bytes"] = total;
                            response["budget"] = state.cacheBudget;
                        } else if (command == "export" || command == "load") {
                            const bool loadOnly = command == "load";
                            const auto file = request.at("zone").get<std::string>();
                            opt.m_output = loadOnly ? std::filesystem::path{} : std::filesystem::path{ request.at("out").get<std::string>() };
                            if (!loadOnly && opt.m_output.empty())
                                throw std::runtime_error("An export request needs an output folder");
                            setRequest(request);
                            state.requestKey = ZoneKey(file);
                            state.lastComplete = false;
                            state.lastError.clear();
                            auto hit = std::find_if(state.cache.begin(), state.cache.end(), [&](const CachedZone& zone) {
                                return zone.key == state.requestKey;
                            });
                            const bool cached = hit != state.cache.end();
                            response["cached"] = cached;
                            if (cached) {
                                state.cache.splice(state.cache.begin(), state.cache, hit);
                                CachedZone& entry = state.cache.front();
                                state.zone = &entry.data;
                                state.streams.reset();
                                if (!loadOnly) {
                                    opt.perfReadMs = opt.perfDecompressMs = opt.perfPatchMs = 0;
                                    // The load position in the manifest is the zone's own (from its load report),
                                    // not that of the zone loaded last.
                                    state.lastLoad = state.loading = state.previous = {};
                                    state.loadMs = state.exportMs = 0;
                                    BeginOutput(opt, entry.name, entry.file);
                                    try {
                                        for (const auto& [key, value] : entry.data.loadReport.items())
                                            state.report[key] = value;
                                        ExportZone(opt);
                                    } catch (const std::exception& error) {
                                        state.lastError = error.what();
                                        state.report["error"] = error.what();
                                        state.Progress();
                                        state.journal.close();
                                    }
                                } else {
                                    state.lastComplete = true;
                                }
                                summary(response);
                            } else {
                                const bool handled = load(file.c_str());
                                if (!handled && state.lastError.empty())
                                    state.lastError = "The fastfile could not be read or decompressed; see the log";
                                summary(response);
                            }
                            response["ok"] = state.lastComplete;
                            if (!state.lastError.empty())
                                response["error"] = state.lastError;
                            state.zone = &state.scratch;
                        } else {
                            throw std::runtime_error("Unknown command: " + command);
                        }
                    } catch (const std::exception& error) {
                        response["ok"] = false;
                        response["error"] = error.what();
                    }
                    state.zone = &state.scratch;
                    state.requestKey.clear();
                    response["ms"] = Tenths(MsSince(start));
                    Respond(response);
                    if (quit)
                        break;
                }
                return true;
            }
        };
        utils::ArrayAdder<Handler, FFHandler> registration{ GetHandlers() };
    } // namespace
} // namespace fastfile::handlers::mw19_replay
