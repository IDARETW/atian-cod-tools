#include "tools/mw19/mw19_schema.hpp"
#include "tools/mw19/mw19_payload.hpp"
#include <zlib.h>
#include <cstring>
#include <iostream>
#include <stdexcept>

using namespace tool::mw19::schema;
void Check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
template<typename Function>
void MustFail(Function f, const char* message) {
    bool failed{};
    try {
        f();
    } catch (const std::exception&) {
        failed = true;
    }
    Check(failed, message);
}
int main(int argc, char** argv) {
    try {
        Check(argc == 2, "schema path required");
        Database db{ std::filesystem::path{ argv[1] } };
        Check(db.Profile("replay-1.20").at("pool_count") == 117, "Replay count");
        Check(db.Profile("game-test").at("pool_count") == 113, "Game-test count");
        Check(db.Pool(db.Profile("replay-1.20"), "stringtable").at("id") == 54, "Replay ordinal");
        Check(db.Pool(db.Profile("game-test"), "stringtable").at("id") == 50, "Source ordinal");
        MustFail([&] { db.Pool(db.Profile("replay-1.20"), "missing"); }, "Unknown pool accepted");
        auto malformed = db.document;
        malformed["profiles"]["replay-1.20"]["pools"][54]["id"] = 55;
        MustFail([&] { Database wrong{ malformed }; }, "Duplicate ordinal accepted");
        malformed = db.document;
        auto& badCases = malformed["union_rules"]["GfxImagePixels"]["cases"];
        badCases.push_back(badCases[0]);
        MustFail([&] { Database wrong{ malformed }; }, "Duplicate union selector accepted");
        malformed = db.document;
        malformed["union_rules"]["GfxImagePixels"]["cases"][0]["external"] = true;
        MustFail([&] { Database wrong{ malformed }; }, "External pointer arm accepted");
        malformed = db.document;
        malformed["pointer_rules"]["WeaponCompleteDef"]["accuracyGraphKnots"]["elements"].push_back(nullptr);
        MustFail([&] { Database wrong{ malformed }; }, "Wrong pointer array cardinality accepted");
        malformed = db.document;
        malformed["pointer_rules"]["WeaponCompleteDef"]["accuracyGraphKnots"]["elements"][0] = Json{ { "count", 1 } };
        MustFail([&] { Database wrong{ malformed }; }, "Pointer array without extent evidence accepted");
        malformed = db.document;
        malformed["pointer_rules"]["TTFDef"]["ftFace"]["count"] = 1;
        MustFail([&] { Database wrong{ malformed }; }, "Conflicting runtime and extent rule accepted");
        constexpr uint64_t base = 0x10000;
        std::vector<uint8_t> memory(8192);
        MemoryReader read = [&](uint64_t at, void* out, size_t length) {
            if (at < base || at - base > memory.size() || length > memory.size() - (at - base))
                return false;
            std::memcpy(out, memory.data() + (at - base), length);
            return true;
        };
        auto put = [&](size_t offset, auto value) { std::memcpy(memory.data() + offset, &value, sizeof(value)); };
        Json small = db.Profile("replay-1.20");
        small["entry_capacity"] = 64;
        small["allocation_flags_offset"] = 1288;
        // Allocate indices 1 and 63; stale bytes in slot 2 must be ignored.
        put(1288, uint64_t{ 0x4000000000000001 });
        put(20, uint64_t{ base + 2000 });
        memory[20 + 17] = 54;
        put(40, uint64_t{ ~uint64_t{ 0 } });
        memory[40 + 17] = 255;
        put(63 * 20, uint64_t{ base + 2100 });
        memory[63 * 20 + 17] = 51;
        auto entries = ReadReplayEntries(read, base, small);
        Check(entries.size() == 2 && entries[0].index == 1 && entries[1].index == 63, "Allocation bit order or stride");
        memory[20 + 17] = 200;
        MustFail([&] { ReadReplayEntries(read, base, small); }, "Invalid pool accepted");
        memory[20 + 17] = 54;
        size_t flagReads{};
        MemoryReader changing = [&](uint64_t at, void* out, size_t length) {
            if (at == base + 1288 && ++flagReads == 2)
                put(1288, uint64_t{ 0 });
            return read(at, out, length);
        };
        MustFail([&] { ReadReplayEntries(changing, base, small); }, "Changing allocation accepted");
        const char name[] = "scripts/example.txt";
        std::memcpy(memory.data() + 3000, name, sizeof(name));
        put(2000, uint64_t{ base + 3000 });
        put(2008, uint32_t{ 0 });
        put(2012, uint32_t{ 12 });
        put(2016, uint64_t{ 0 });
        Inspector inspector(db, read);
        auto raw = inspector.Inspect("RawFile", base + 2000);
        Check(raw.at("read_errors") == 0, "RawFile inspection failed");
        Check(raw["fields"]["name"]["string"] == name && raw["fields"]["len"] == 12, "Incorrect rawfile offsets");
        const char content[] = "hello, raw\n";
        std::memcpy(memory.data() + 3200, content, sizeof(content) - 1);
        put(2012, uint32_t{ sizeof(content) - 1 });
        put(2016, uint64_t{ base + 3200 });
        auto payload = ExportPayload(read, "rawfile", base + 2000);
        Check(std::string(payload.data.begin(), payload.data.end()) == content, "Uncompressed rawfile");
        std::vector<uint8_t> compressed(compressBound(sizeof(content) - 1));
        uLongf compressedSize = static_cast<uLongf>(compressed.size());
        Check(
            compress(
                compressed.data(),
                &compressedSize,
                reinterpret_cast<const Bytef*>(content),
                sizeof(content) - 1
            ) == Z_OK,
            "Fixture compression"
        );
        std::memcpy(memory.data() + 3400, compressed.data(), compressedSize);
        put(2008, static_cast<uint32_t>(compressedSize));
        put(2016, uint64_t{ base + 3400 });
        payload = ExportPayload(read, "rawfile", base + 2000);
        Check(std::string(payload.data.begin(), payload.data.end()) == content, "Compressed rawfile");
        put(2012, uint32_t{ 100 });
        MustFail([&] { ExportPayload(read, "rawfile", base + 2000); }, "Bad decompressed length accepted");
        put(2008, int32_t{ -1 });
        MustFail([&] { ExportPayload(read, "rawfile", base + 2000); }, "Negative compressed size accepted");
        // A 2x1 stringtable with a quoted cell exercises dictionary indirection
        // and CSV escaping, not a memcpy of a guessed StringTableCell layout.
        put(2208, int32_t{ 2 });
        put(2212, int32_t{ 1 });
        put(2216, int32_t{ 2 });
        put(2224, uint64_t{ base + 3600 });
        put(2240, uint64_t{ base + 3700 });
        put(3600, uint16_t{ 1 });
        put(3602, uint16_t{ 0 });
        put(3700, uint64_t{ base + 3800 });
        put(3708, uint64_t{ base + 3900 });
        std::memcpy(memory.data() + 3800, "a\"b", 4);
        std::memcpy(memory.data() + 3900, "first", 6);
        payload = ExportPayload(read, "stringtable", base + 2200);
        Check(
            std::string(payload.data.begin(), payload.data.end()) == "\"first\",\"a\"\"b\"\n",
            "CSV cell order or escaping"
        );
        put(3602, uint16_t{ 3 });
        MustFail([&] { ExportPayload(read, "stringtable", base + 2200); }, "Invalid dictionary index accepted");
        put(3602, uint16_t{ 0 });
        auto tableFields = inspector.Inspect("StringTable", base + 2200);
        Check(tableFields.at("read_errors") == 0, "Stringtable traversal failed");
        Check(
            tableFields["fields"]["cellIndices"]["values"].size() == 2 &&
                tableFields["fields"]["cellIndices"]["values"][0] == 1,
            "Pointer array extent or element read"
        );
        Check(tableFields["fields"]["strings"]["values"][1]["string"] == "first", "String pointer array traversal");
        put(2212, int32_t{ 2 });
        put(3600, uint16_t{ 0 });
        put(3602, uint16_t{ 1 });
        put(3604, uint16_t{ 0 });
        put(3606, uint16_t{ 0 });
        payload = ExportPayload(read, "stringtable", base + 2200);
        Check(
            std::string(payload.data.begin(), payload.data.end()) == "\"a\"\"b\",\"a\"\"b\"\n\"first\",\"a\"\"b\"\n",
            "Column-major CSV addressing"
        );
        put(2212, int32_t{ 1 });
        put(2208, int32_t{ -1 });
        Check(
            inspector.Inspect("StringTable", base + 2200).at("read_errors").get<size_t>() > 0,
            "Unbounded negative array extent"
        );
        put(2208, int32_t{ 2 });
        // Replay no longer contains Camo's two source vehicle FX fields.
        // Put unreadable addresses exactly at the source-only offsets: the
        // Replay inspector must never try to follow them.
        std::fill(memory.begin() + 4000, memory.begin() + 5000, 0);
        put(4000, uint64_t{ base + 3000 });
        put(4000 + 168, uint64_t{ 0xdeadbeef });
        put(4000 + 176, uint64_t{ 0xdeadbeef });
        Inspector replayInspector(db, read, {}, "replay-1.20");
        auto camoFields = replayInspector.Inspect("Camo", base + 4000);
        Check(
            camoFields.at("read_errors") == 0 && !camoFields["fields"].contains("vehVfxTailLight"),
            "Replay Camo used source-only fields"
        );
        // Location damage moved from +0x258 to +0x250 in Replay attachment.
        std::fill(memory.begin() + 4000, memory.begin() + 5000, 0);
        put(4000 + 0x250, uint64_t{ base + 5100 });
        put(5100, float{ 1.25f });
        auto attachmentFields = replayInspector.Inspect("WeaponAttachment", base + 4000);
        Check(attachmentFields.at("read_errors") == 0, "Replay attachment traversal failed");
        Check(
            attachmentFields["fields"]["locationDamage"]["values"][0] == 1.25f,
            "Replay locationDamage offset or count"
        );
        Check(
            !attachmentFields["fields"].contains("weaponOffsetPatternScaleInfo"),
            "Replay attachment retained absent pointer"
        );
        // Every nonempty catalog root is traversable with empty payloads.
        // This checks native root sizes, embedded strides and type references;
        // it is a synthetic layout check, not a live asset-content proof.
        std::vector<uint8_t> empty(65536);
        MemoryReader zero = [&](uint64_t at, void* out, size_t length) {
            if (at < base || at - base > empty.size() || length > empty.size() - (at - base))
                return false;
            std::memcpy(out, empty.data() + at - base, length);
            return true;
        };
        size_t checkedRoots{};
        for (const auto& pool : db.Profile("replay-1.20").at("pools")) {
            if (!pool.at("size").get<size_t>())
                continue;
            Check(!pool.at("root_type").is_null(), "Missing Replay root");
            Inspector rootInspector(db, zero, {}, "replay-1.20");
            auto root = rootInspector.Inspect(pool.at("root_type"), base);
            if (root.at("read_errors") != 0)
                throw std::runtime_error(
                    "Empty root traversal failed: " + pool.at("name").get<std::string>() + " " + root.dump()
                );
            checkedRoots++;
        }
        Check(checkedRoots == 112, "Nonempty Replay root coverage");
        // Vectors and flag unions contain value views, not alternative
        // allocations. Their numeric contents must remain available.
        put(6000, float{ 1.0f });
        put(6004, float{ 2.5f });
        put(6008, float{ -3.0f });
        auto vector = replayInspector.Inspect("vec3_t", base + 6000);
        Check(vector["unresolved_unions"] == 0 && vector["fields"]["v"][1] == 2.5f, "Numeric union views lost");
        // XAnimIndices changes element width at the native 256-frame boundary.
        std::fill(memory.begin() + 6000, memory.begin() + 7200, 0);
        put(6000 + 0x40, uint64_t{ base + 6500 });
        put(6000 + 0x60, uint32_t{ 3 });
        put(6000 + 0x76, uint16_t{ 255 });
        memory[6500] = 7;
        memory[6501] = 8;
        memory[6502] = 9;
        auto animation = replayInspector.Inspect("XAnimParts", base + 6000);
        Check(
            animation["read_errors"] == 0 && animation["fields"]["indices"]["active_member"] == "_1" &&
                animation["fields"]["indices"]["value"]["bytes"] == "070809",
            "Byte animation indices"
        );
        put(6000 + 0x76, uint16_t{ 256 });
        put(6500, uint16_t{ 257 });
        put(6502, uint16_t{ 258 });
        put(6504, uint16_t{ 259 });
        animation = replayInspector.Inspect("XAnimParts", base + 6000);
        Check(
            animation["read_errors"] == 0 && animation["fields"]["indices"]["active_member"] == "_2" &&
                animation["fields"]["indices"]["value"]["values"][2] == 259,
            "Word animation indices"
        );
        // A streamed image handle deliberately contains an unreadable value.
        // Selecting that union arm must preserve it without dereferencing it.
        std::fill(memory.begin() + 6000, memory.begin() + 6400, 0);
        put(6000 + 0x1c, uint32_t{ 4 });
        put(6000 + 0xe0, uint64_t{ base + 6500 });
        std::memcpy(memory.data() + 6500, "RGBA", 4);
        auto picture = replayInspector.Inspect("GfxImage", base + 6000);
        Check(
            picture["read_errors"] == 0 && picture["fields"]["pixels"]["value"]["bytes"] == "52474241",
            "Resident image bytes"
        );
        put(6000 + 0x18, uint32_t{ 0x40 });
        put(6000 + 0xe0, uint64_t{ 0x123456789abcdef0 });
        picture = replayInspector.Inspect("GfxImage", base + 6000);
        Check(
            picture["read_errors"] == 0 && picture["external_payloads"] == 1 &&
                picture["fields"]["pixels"]["active_member"] == "streamedDataHandle",
            "Streamed image handle followed as a pointer"
        );
        std::fill(memory.begin() + 6000, memory.begin() + 6400, 0);
        put(6000 + 0x40, uint32_t{ 17 });
        put(6000 + 0x48, uint64_t{ 0xdeadbeef });
        auto unknownAsm = replayInspector.Inspect("ASM", base + 6000);
        Check(
            unknownAsm["read_errors"] == 0 && unknownAsm["unresolved_unions"] == 1 &&
                unknownAsm["fields"]["u"]["status"] == "unknown_selector",
            "Unknown ASM variant followed"
        );
        // Every serialized event/module case selects its declared member.
        // Nonzero nested pointer fixtures below additionally prove traversal.
        for (const auto& item : std::vector<std::tuple<std::string, size_t, std::string, std::string>>{
                 { "ScriptableEventDef", 16, "data", "ScriptableEventDefUnion" },
                 { "ScriptableStateDef", 24, "data", "ScriptableStateDefUnion" },
                 { "PhysicsSFXEventAssetRule", 0, "u", "PhysicsSFXEventAssetRuleUnion" },
                 { "PhysicsVFXEventAssetRule", 0, "u", "PhysicsVFXEventAssetRuleUnion" },
                 { "ParticleModuleDef", 0, "moduleData", "ParticleModuleTypeDef" } }) {
            const auto& [owner, offset, member, unionType] = item;
            for (const auto& arm : db.document["union_rules"][unionType]["cases"]) {
                std::fill(memory.begin() + 6000, memory.begin() + 7200, 0);
                put(6000 + offset, arm["value"].get<uint32_t>());
                auto record = replayInspector.Inspect(owner, base + 6000);
                if (record["read_errors"] != 0 || record["fields"][member]["active_member"] != arm["member"])
                    throw std::runtime_error("Union case traversal: " + owner + " " + arm.dump() + " " + record.dump());
            }
        }
        std::fill(memory.begin() + 6000, memory.begin() + 7200, 0);
        put(6016, uint32_t{ 13 });          // Scriptable_EventType_Sound
        put(6056, uint64_t{ base + 7000 }); // data.sound.soundAlias
        std::memcpy(memory.data() + 7000, "fixture_sound", 14);
        auto event = replayInspector.Inspect("ScriptableEventDef", base + 6000);
        Check(
            event["read_errors"] == 0 && event["fields"]["data"]["value"]["soundAlias"]["string"] == "fixture_sound",
            "Scriptable sound union pointer not traversed"
        );
        std::fill(memory.begin() + 6000, memory.begin() + 6900, 0);
        put(6000, uint16_t{ 22 }); // ParticleModuleDef INIT_SOUND
        put(6032, uint64_t{ base + 6600 });
        put(6040, uint32_t{ 1 });
        put(6600, uint64_t{ base + 7000 });
        auto module = replayInspector.Inspect("ParticleModuleDef", base + 6000);
        auto link = module["fields"]["moduleData"]["value"]["m_linkedAssetList"]["assetList"]["values"][0];
        Check(
            module["read_errors"] == 0 && link["active_member"] == "sound" &&
                link["value"]["string"] == "fixture_sound",
            "Particle linked sound read as one byte"
        );
        // A three-element inline delta index tail exceeds its declared [1].
        // Quaternion and translation contexts select their own count fields.
        for (bool translation : { false, true }) {
            for (uint16_t frames : { uint16_t{ 255 }, uint16_t{ 256 } }) {
                std::fill(memory.begin() + 6000, memory.begin() + 7200, 0);
                put(6080, uint64_t{ base + 6300 });
                put(6118, frames);
                put(6300 + (translation ? 0 : 16), uint64_t{ base + 6400 });
                put(6400, uint16_t{ 2 });
                const size_t at = translation ? 6440 : 6416;
                if (frames < 256) {
                    memory[at] = 4;
                    memory[at + 1] = 5;
                    memory[at + 2] = 6;
                } else {
                    put(at, uint16_t{ 260 });
                    put(at + 2, uint16_t{ 261 });
                    put(at + 4, uint16_t{ 262 });
                }
                auto anim = replayInspector.Inspect("XAnimParts", base + 6000);
                auto indices = anim["fields"]["deltaPart"]["values"][0][translation ? "trans" : "quat"]["values"][0]
                                   ["u"]["value"]["indices"];
                Check(
                    anim["read_errors"] == 0 && indices["count"] == 3 &&
                        indices["value"][2] == (frames < 256 ? 6 : 262),
                    "Inline delta animation indices truncated or wrong context"
                );
            }
        }
        std::fill(memory.begin() + 6000, memory.begin() + 7200, 0);
        put(6040, uint64_t{ base + 7000 });
        put(6056, uint32_t{ 4 });
        put(6061, uint8_t{ 2 });
        put(6048, uint64_t{ 0x1234567812345678 });
        std::memcpy(memory.data() + 7000, "STRM", 4);
        auto stream = replayInspector.Inspect("StreamKey", base + 6000);
        Check(
            stream["read_errors"] == 0 && stream["unresolved_unions"] == 0 &&
                stream["fields"]["data"]["value"]["bytes"] == "5354524d" &&
                stream["fields"]["___u3"]["active_member"] == "assetHash" &&
                stream["fields"]["___u3"]["value"] == uint64_t{ 0x1234567812345678 },
            "Resident stream key data"
        );
        for (uint8_t behavior = 1; behavior < 5; ++behavior) {
            put(6060, behavior);
            stream = replayInspector.Inspect("StreamKey", base + 6000);
            Check(
                stream["read_errors"] == 0 && stream["unresolved_unions"] == 0 && stream["runtime_references"] == 1 &&
                    stream["fields"]["___u3"]["active_member"] == "behaviorUserPtr" &&
                    stream["fields"]["___u3"]["value"]["status"] == "runtime_reference",
                "Active behavior context was followed or interpreted as a hash"
            );
        }
        put(6060, uint8_t{ 5 });
        stream = replayInspector.Inspect("StreamKey", base + 6000);
        Check(
            stream["read_errors"] == 0 && stream["unresolved_unions"] == 1 &&
                stream["fields"]["___u3"]["status"] == "unknown_selector",
            "Unknown behavior selector was hidden as a known runtime context"
        );
        put(6060, uint8_t{});
        put(6040, uint64_t{});
        stream = replayInspector.Inspect("StreamKey", base + 6000);
        Check(
            stream["read_errors"] == 0 && stream["external_payloads"] == 1 &&
                stream["fields"]["data"]["status"] == "resident_payload_unavailable",
            "Missing nonempty stream buffer omitted"
        );
        put(6040, uint64_t{ 0xdeadbeef });
        put(6061, uint8_t{ 0 });
        stream = replayInspector.Inspect("StreamKey", base + 6000);
        Check(stream["read_errors"] == 0 && stream["external_payloads"] == 1, "Stream key handle dereferenced");
        std::fill(memory.begin() + 6000, memory.begin() + 6020, 0);
        put(6008, uint32_t{ 4 });
        auto shared = replayInspector.Inspect("XSurfaceShared", base + 6000);
        Check(
            shared["read_errors"] == 0 && shared["external_payloads"] == 1 &&
                shared["fields"]["data"]["status"] == "resident_payload_unavailable",
            "Missing nonempty shared geometry omitted"
        );
        // Streaming content lengths reside in a sibling array, indexed by
        // this record's position, with a separate table for each stream set.
        std::fill(memory.begin() + 6000, memory.begin() + 7200, 0);
        put(6076, uint32_t{ 2 });
        put(6000, uint64_t{ base + 6200 });
        put(6008, uint64_t{ base + 6240 });
        put(6200, uint16_t{ 1 });
        put(6202, uint16_t{ 3 });
        put(6240, uint64_t{ base + 6280 });
        put(6248, uint64_t{ base + 6300 });
        put(6280, uint16_t{ 17 });
        put(6300, uint16_t{ 21 });
        put(6302, uint16_t{ 22 });
        put(6304, uint16_t{ 23 });
        put(6016, uint64_t{ base + 6340 });
        put(6024, uint64_t{ base + 6380 });
        put(6340, uint16_t{ 2 });
        put(6342, uint16_t{ 1 });
        put(6380, uint64_t{ base + 6420 });
        put(6388, uint64_t{ base + 6440 });
        put(6420, uint16_t{ 31 });
        put(6422, uint16_t{ 32 });
        put(6440, uint16_t{ 41 });
        auto costs = replayInspector.Inspect("TransientCosts", base + 6000);
        auto firstSet = costs["fields"]["streamContents"][0]["contents"]["values"];
        auto secondSet = costs["fields"]["streamContents"][1]["contents"]["values"];
        Check(
            costs["read_errors"] == 0 && firstSet[1]["contents"]["count"] == 3 &&
                firstSet[1]["contents"]["values"][2] == 23 && secondSet[0]["contents"]["count"] == 2 &&
                secondSet[1]["contents"]["values"][0] == 41,
            "Streaming content count lookup used wrong table or index"
        );
        put(6000, uint64_t{ 0 });
        costs = replayInspector.Inspect("TransientCosts", base + 6000);
        Check(costs["read_errors"].get<size_t>() > 0, "Missing streaming count table accepted");
        std::fill(memory.begin() + 6000, memory.begin() + 7200, 0);
        put(6008, uint64_t{ base + 7000 });
        put(6024, uint32_t{ 0x200 });
        put(6044, uint16_t{ 4 });
        std::memcpy(memory.data() + 7000, "ATLS", 4);
        auto atlas = replayInspector.Inspect("GfxImage", base + 6000);
        Check(
            atlas["read_errors"] == 0 && atlas["fields"]["packedAtlasData"]["bytes"] == "41544c53",
            "Packed atlas extent"
        );
        std::fill(memory.begin() + 6000, memory.begin() + 7200, 0);
        put(6008, uint64_t{ base + 6100 });
        put(6016, uint64_t{ base + 6200 });
        put(6100, int32_t{ -1 });
        put(6108, int32_t{ 2 });
        put(6116, uint64_t{ base + 6300 });
        put(6200, int32_t{ -1 });
        put(6208, int32_t{ 1 });
        put(6216, uint64_t{ base + 6310 });
        put(6300, uint16_t{ 101 });
        put(6302, uint16_t{ 102 });
        put(6310, uint16_t{ 201 });
        auto tree = replayInspector.Inspect("pathnode_tree_t", base + 6000);
        Check(
            tree["read_errors"] == 0 && tree["unresolved_pointers"] == 0 &&
                tree["fields"]["u"]["value"][1]["values"][0]["u"]["value"]["nodes"]["values"][0] == 201,
            "Path tree branch/leaf union traversal"
        );
        std::fill(memory.begin() + 6000, memory.begin() + 7200, 0);
        put(6024, int32_t{ 3 });
        put(6032, uint64_t{ base + 6300 });
        put(6300, uint32_t{ 0x12345678 });
        put(6304, int32_t{ 0 });
        put(6308, int32_t{ -9 });
        auto bundle = replayInspector.Inspect("ScriptBundle", base + 6000);
        Check(
            bundle["read_errors"] == 0 && bundle["fields"]["___u4"]["value"]["count"] == 3 &&
                bundle["fields"]["___u4"]["value"]["values"][2] == -9,
            "Script bundle root words truncated at zero"
        );
        // Each pointer in a fixed array has its own loader-defined extent.
        std::fill(memory.begin() + 6000, memory.end(), 0);
        put(6548, uint16_t{ 1 });
        put(6550, uint16_t{ 2 });
        put(6552, uint64_t{ base + 7500 });
        put(6560, uint64_t{ base + 7600 });
        put(7500, 1.25f);
        put(7504, 2.5f);
        put(7600, 3.0f);
        put(7604, 4.0f);
        put(7608, 5.0f);
        put(7612, 6.0f);
        auto weapon = replayInspector.Inspect("WeaponCompleteDef", base + 6000);
        const auto& curves = weapon["fields"]["accuracyGraphKnots"];
        Check(
            weapon["read_errors"] == 0 && curves[0]["count"] == 1 && curves[1]["count"] == 2 &&
                curves[1]["values"][1]["v"][1] == 6.0,
            "Weapon curves used one extent for both slots"
        );
        std::fill(memory.begin() + 6000, memory.end(), 0);
        put(6000, uint32_t{ 2 });
        put(6004, uint32_t{ 1 });
        put(6032, uint64_t{ base + 7500 });
        put(6032 + 33 * 8, uint64_t{ base + 7600 });
        put(6032 + 3 * 8, uint64_t{ 0xdeadbeef }); // The serialized extent is zero for this slot.
        std::fill(memory.begin() + 7500, memory.begin() + 7564, uint8_t{ 0x5a });
        std::fill(memory.begin() + 7600, memory.begin() + 7632, uint8_t{ 0xa5 });
        auto visibility = replayInspector.Inspect("GfxWorldDpvsDynamic", base + 6000);
        auto views = visibility["fields"]["dynEntVisData"];
        Check(
            visibility["read_errors"] == 0 && visibility["unresolved_pointers"] == 0 && views[0][0]["count"] == 64 &&
                views[1][0]["count"] == 32 && views[0][3]["bytes"] == "" &&
                views[1][0]["bytes"].get<std::string>().starts_with("a5a5"),
            "Nested pointer arrays lost per-row or zero extents"
        );
        put(6032 + 32 * 8, uint64_t{ base + 7700 });
        visibility = replayInspector.Inspect("GfxWorldDpvsDynamic", base + 6000);
        Check(
            visibility["read_errors"] == 0 && visibility["fields"]["dynEntVisData"][0][32]["count"] == 64,
            "Runtime view 32 extent was not inspected"
        );
        auto missingSlotSchema = db.document;
        missingSlotSchema["pointer_rules"]["GfxWorldDpvsDynamic"]["dynEntVisData"]["elements"][0]["elements"][32] =
            nullptr;
        Database missingSlotDb(missingSlotSchema);
        Inspector missingSlotInspector(missingSlotDb, read, {}, "replay-1.20");
        visibility = missingSlotInspector.Inspect("GfxWorldDpvsDynamic", base + 6000);
        Check(
            visibility["read_errors"] == 0 && visibility["unresolved_pointers"] == 1 &&
                visibility["issues"][0]["path"] == "fields/dynEntVisData/0/32",
            "Unmapped array slot was hidden or dereferenced"
        );
        put(6032, uint64_t{ 0xdeadbeef });
        visibility = replayInspector.Inspect("GfxWorldDpvsDynamic", base + 6000);
        Check(
            visibility["read_errors"] == 1 && visibility["fields"]["dynEntVisData"][1][0]["count"] == 32,
            "One unreadable pointer array slot aborted its siblings"
        );
        // Replay's technique count is popcount over all four 64-bit words.
        std::fill(memory.begin() + 6000, memory.end(), 0);
        put(6024, uint64_t{ 1 } << 63);
        put(6048, uint64_t{ 3 });
        put(6056, uint64_t{ base + 6500 });
        for (size_t i = 0; i < 3; ++i)
            put(6500 + 8 * i, uint64_t{ base + 6600 + 200 * i });
        auto techniques = replayInspector.Inspect("MaterialTechniqueSet", base + 6000);
        auto techniquePointers = techniques["fields"]["maskedTechniques"];
        Check(
            techniques["read_errors"] == 0 && techniques["unresolved_pointers"] == 0 &&
                techniquePointers["count"] == 3 && techniquePointers["values"][2]["values"].size() == 1,
            "Technique mask population or pointer-to-pointer traversal"
        );
        std::fill(memory.begin() + 6000, memory.end(), 0);
        put(6008, uint64_t{ base + 6500 });
        put(6016, int32_t{ 2 });
        put(6500, uint64_t{ base + 6600 });
        put(6508, uint64_t{ base + 6700 });
        put(6608, uint64_t{ base + 7100 });
        put(6708, uint64_t{ base + 7120 });
        std::memcpy(memory.data() + 7100, "serializer_one", 15);
        std::memcpy(memory.data() + 7120, "serializer_two", 15);
        put(6616, uint64_t{ 0xdeadbeef }); // A callback stays a runtime reference.
        auto channel = replayInspector.Inspect("DLogChannel", base + 6000);
        Check(
            channel["read_errors"] == 0 && channel["unresolved_pointers"] == 0 &&
                channel["fields"]["serializers"]["values"][1]["values"][0]["name"]["string"] == "serializer_two",
            "Serializer pointer list did not traverse individual records"
        );
        std::fill(memory.begin() + 6000, memory.end(), 0);
        put(6024, uint64_t{ base + 6500 });
        put(6500, uint64_t{ base + 6000 });
        auto tactical = replayInspector.Inspect("tacpoint_search_node_t", base + 6000);
        Check(
            tactical["read_errors"] == 0 && tactical["unresolved_pointers"] == 0 &&
                tactical["fields"]["m_ChildNodes"]["count"] == 4 &&
                tactical["fields"]["m_ChildNodes"]["values"][0]["status"] == "reference_already_exported",
            "Pointer-to-pointer cycle did not terminate as a reference"
        );
        // Each cell has its own count record; the second has two AABB records.
        std::fill(memory.begin() + 6000, memory.end(), 0);
        put(6000, uint32_t{ 2 });
        put(6008, uint64_t{ base + 6100 });
        put(6016, uint64_t{ base + 6200 });
        put(6100, int32_t{ 1 });
        put(6104, int32_t{ 2 });
        put(6200, uint64_t{ base + 6300 });
        put(6208, uint64_t{ base + 6500 });
        put(6576, uint32_t{ 77 });
        put(6586, uint16_t{ 2 });
        put(6588, uint64_t{ base + 7000 });
        put(7000, uint16_t{ 12 });
        put(7002, uint16_t{ 34 });
        auto cells = replayInspector.Inspect("GfxWorldDrawCells", base + 6000);
        auto cellValues = cells["fields"]["aabbTrees"]["values"];
        Check(
            cells["read_errors"] == 0 && cells["unresolved_pointers"] == 0 && cellValues[0]["aabbTree"]["count"] == 1 &&
                cellValues[1]["aabbTree"]["count"] == 2 &&
                cellValues[1]["aabbTree"]["values"][1]["startSurfIndex"] == 77 &&
                cellValues[1]["aabbTree"]["values"][1]["smodelIndexes"]["values"][1] == 34,
            "Parallel cell count records were indexed incorrectly"
        );
        put(6104, int32_t{ -1 });
        cells = replayInspector.Inspect("GfxWorldDrawCells", base + 6000);
        Check(
            cells["read_errors"] == 1 && cells["fields"]["aabbTrees"]["values"][0]["aabbTree"]["count"] == 1,
            "Negative cell extent did not fail independently"
        );
        put(6008, uint64_t{ 0 });
        cells = replayInspector.Inspect("GfxWorldDrawCells", base + 6000);
        Check(cells["read_errors"] == 2, "Missing cell count array accepted");
        Check(
            replayInspector.Inspect("GfxCellTree", base + 6200)["read_errors"] == 1,
            "Cell tree without its owner count context accepted"
        );
        // Runtime references retain provenance but must never be followed.
        std::fill(memory.begin() + 6000, memory.end(), 0);
        put(6024, uint64_t{ 0xdeadbeef });
        auto fontRuntime = replayInspector.Inspect("TTFDef", base + 6000);
        Check(
            fontRuntime["read_errors"] == 0 && fontRuntime["unresolved_pointers"] == 0 &&
                fontRuntime["runtime_references"] == 1 && fontRuntime["fields"]["ftFace"]["address"] == "0xdeadbeef" &&
                fontRuntime["fields"]["ftFace"]["serialized_source"] == "TTFDef.file",
            "FreeType runtime object was followed or lost its source annotation"
        );
        std::fill(memory.begin() + 6000, memory.end(), 0);
        put(6008, uint64_t{ 0xdeadbeef });
        auto modelRuntime = replayInspector.Inspect("XModelLodInfo", base + 6000);
        Check(
            modelRuntime["read_errors"] == 0 && modelRuntime["runtime_references"] == 1 &&
                modelRuntime["fields"]["surfs"]["status"] == "runtime_reference",
            "Derived model surface alias was treated as owned data"
        );
        std::fill(memory.begin() + 6000, memory.end(), 0);
        put(6000, uint64_t{ 0xdeadbeef });
        put(6008, uint64_t{ 0xf00d });
        auto equipmentRuntime = replayInspector.Inspect("EquipmentSoundSet", base + 6000);
        Check(
            equipmentRuntime["read_errors"] == 0 && equipmentRuntime["runtime_references"] == 2,
            "Equipment sound runtime aliases were followed"
        );
        std::fill(memory.begin() + 6000, memory.end(), 0);
        put(6000, uint64_t{ base + 6500 });
        put(6008, uint32_t{ 2 });
        put(6012, uint32_t{ 40 });
        put(6500, uint32_t{ 0x80000001 });
        put(6504, uint32_t{ 0xff000000 });
        auto bits = replayInspector.Inspect("bitarray_dynamic", base + 6000);
        Check(
            bits["read_errors"] == 0 && bits["fields"]["array"]["count"] == 2 &&
                bits["fields"]["array"]["values"][1] == 0xff000000u,
            "Dynamic bit array used bit count as a word extent"
        );
        // Shader load definitions moved from +32 in the source compute
        // shader to +24 in every native Replay shader stage.
        std::fill(memory.begin() + 4000, memory.begin() + 4200, 0);
        put(4024, uint64_t{ base + 5200 });
        put(4032, uint32_t{ 4 });
        std::memcpy(memory.data() + 5200, "DXBC", 4);
        for (const auto* stage :
             { "computeshader", "libshader", "vertexshader", "hullshader", "domainshader", "pixelshader" }) {
            auto shader = ExportPayload(read, stage, base + 4000);
            Check(std::string(shader.data.begin(), shader.data.end()) == "DXBC", "Replay shader program offset");
        }
        put(4032, uint64_t{ base + 5200 });
        put(4040, uint32_t{ 4 });
        auto sourceShader = ExportPayload(read, "computeshader", base + 4000, 1024, "game-test");
        Check(sourceShader.data.size() == 4, "Source shader program offset");
        put(4024, uint64_t{ 0 });
        MustFail([&] { ExportPayload(read, "vertexshader", base + 4000); }, "Nonresident shader accepted");
        Check(
            inspector.Inspect("RawFile", 0xdeadbeef).at("read_errors").get<unsigned>() > 0,
            "Unreadable memory hidden"
        );
        Limits tiny;
        tiny.maxBytes = 4;
        Inspector limited(db, read, tiny);
        Check(limited.Inspect("RawFile", base + 2000).at("read_errors").get<unsigned>() > 0, "Byte budget ignored");
        Check(ReadString(read, base + 3000) == name, "String read");
        MustFail([&] { ReadString(read, base + 3000, 3); }, "Unterminated string accepted");
        for (const auto* path : { "../escape",
                                  "C:/escape",
                                  "//server/share",
                                  "maps/../bad",
                                  "file:ads",
                                  "a./b",
                                  "NUL",
                                  "con.txt",
                                  "maps/COM1/file" }) {
            MustFail([&] { AssetPath("output", path); }, "Unsafe path accepted");
        }
        Check(
            AssetPath("output", "scripts\\example.gsc").generic_string() == "output/scripts/example.gsc",
            "Path normalization"
        );
        // The structured encoder preserves bytes and rejects incomplete traversal.
        std::fill(memory.begin() + 6000, memory.end(), 0);
        put(6000, uint64_t{ base + 7000 });
        memory[7000] = 0xff;
        memory[7001] = 'A';
        auto record = replayInspector.Inspect("WeaponCompleteDef", base + 6000);
        Check(record["fields"]["szInternalName"]["string_bytes"] == "ff41", "Opaque string bytes lost");
        const auto& replay = db.Profile("replay-1.20");
        const auto& weaponPool = db.Pool(replay, "weapon");
        auto encoded = ExportStructuredAsset(replay, weaponPool, record);
        auto decoded = Json::parse(encoded.data);
        Check(decoded["asset"] == record && encoded.format == "mw19-asset-json-v1", "Structured round trip failed");
        MustFail([&] { ExportStructuredAsset(replay, weaponPool, record, 8); }, "Output budget ignored");
        for (const char* counter : { "read_errors", "unresolved_pointers", "unresolved_unions", "external_payloads" }) {
            auto incomplete = record;
            incomplete[counter] = 1;
            MustFail(
                [&] { ExportStructuredAsset(replay, weaponPool, incomplete); },
                "Incomplete structured asset accepted"
            );
        }
        MustFail([&] { ExportStructuredAsset(replay, db.Pool(replay, "rawfile"), record); }, "Wrong pool accepted");
        // Populated Replay-only offsets and integer bitset rounding. These
        // were exposed by stock fastfiles, not by zero-filled root fixtures.
        std::fill(memory.begin(), memory.end(), 0);
        put(6008, uint16_t{ 2 });
        put(6016, uint64_t{ base + 6500 });
        put(6500, uint16_t{ 1 });
        put(6502, uint16_t{ 12 });
        put(6508, uint64_t{ base + 7000 });
        memory[7000] = 0xab;
        memory[7001] = 0xcd;
        auto tacticalGraph = replayInspector.Inspect("TacticalGraph", base + 6000);
        Check(
            tacticalGraph["read_errors"] == 0 && tacticalGraph["fields"]["m_VisGraph"]["count"] == 1 &&
                tacticalGraph["fields"]["m_VisGraph"]["values"][0]["m_Vis"]["bytes"] == "abcd",
            "Tactical graph row count or partial visibility byte was lost"
        );
        std::fill(memory.begin(), memory.end(), 0);
        put(512 + 1904, uint64_t{ base + 7000 });
        put(7000, float{ 12.5 });
        put(512 + 2852, float{ 0.75 });
        auto replayWeapon = replayInspector.Inspect("WeaponDef", base + 512);
        Check(
            replayWeapon["read_errors"] == 0 && replayWeapon["fields"]["parallelBounce"]["values"][0] == 12.5 &&
                replayWeapon["fields"]["weaponOffsetCurveHoldFireSlow"]["blendTime"] == 0.75 &&
                !replayWeapon["fields"].contains("hyperBurstInfo"),
            "Replay WeaponDef offsets"
        );
        // Bulk arrays preserve exact bits and element layout, stay within
        // byte limits, and do not spend one traversal node per scalar.
        memory.assign(32768, 0);
        put(0, uint64_t{ base + 1024 });
        put(8, uint32_t{ 4096 });
        put(12, uint32_t{ 131072 });
        put(1024, uint32_t{ 0x1234abcd });
        put(1024 + 4095 * 4, uint32_t{ 0xfedcba98 });
        Limits bulkLimits;
        bulkLimits.compactArrays = true;
        bulkLimits.maxNodes = 32;
        Inspector bulkInspector(db, read, bulkLimits, "replay-1.20");
        auto bulk = bulkInspector.Inspect("bitarray_dynamic", base);
        const auto& array = bulk["fields"]["array"];
        auto hex = array.at("bytes").get<std::string>();
        Check(
            bulk["read_errors"] == 0 && array["encoding"] == "hex-little-endian" && array["stride"] == 4 &&
                hex.size() == 4096 * 8 && hex.starts_with("cdab3412") && hex.ends_with("98badcfe"),
            "Compact typed array did not preserve complete bytes"
        );
        bulkLimits.maxBytes = 1000;
        Inspector bulkLimited(db, read, bulkLimits, "replay-1.20");
        Check(bulkLimited.Inspect("bitarray_dynamic", base)["read_errors"] == 1, "Compact array bypassed byte bounds");
        std::cout << "MW2019 schema, 112 Replay roots, version separation, pointer arrays, payloads, allocation, "
                     "corruption, bounds and path checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
