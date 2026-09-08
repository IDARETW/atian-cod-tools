// A synthetic process for the real read-only CLI. This contains no game code.
#define NOMINMAX
#include <Windows.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <zlib.h>

using Json = nlohmann::ordered_json;
std::vector<std::unique_ptr<uint8_t[]>> allocations;
uint64_t Buffer(size_t size) {
    auto data = std::make_unique<uint8_t[]>(size ? size : 1);
    auto address = reinterpret_cast<uint64_t>(data.get());
    allocations.push_back(std::move(data));
    return address;
}
template<typename T>
void Put(uint64_t at, T value) {
    std::memcpy(reinterpret_cast<void*>(at), &value, sizeof(value));
}
uint64_t Text(const std::string& text) {
    auto at = Buffer(text.size() + 1);
    std::memcpy(reinterpret_cast<void*>(at), text.c_str(), text.size() + 1);
    return at;
}
void Map(uint64_t at, size_t size) {
    if (VirtualAlloc(reinterpret_cast<void*>(at), size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) !=
        reinterpret_cast<void*>(at)) {
        throw std::runtime_error("Cannot allocate synthetic table mapping");
    }
}

int main(int argc, char** argv) {
    try {
        if (argc != 4 && argc != 5)
            throw std::runtime_error("schema, ready, stop and optional streamed-image specification paths required");
        Json imageSpec;
        if (argc == 5) {
            std::ifstream spec(argv[4]);
            spec >> imageSpec;
        }
        std::ifstream in(argv[1]);
        Json schema;
        in >> schema;
        const auto& profile = schema.at("profiles").at("replay-1.20");
        const auto base = reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));
        Map(base + 0x2450000, 0x10000);
        Map(base + 0xC810000, 0x700000);
        const auto entries = base + profile.at("entry_table_rva").get<uint64_t>();
        const auto flags = entries + profile.at("allocation_flags_offset").get<uint64_t>();
        const std::vector<std::string> pools = {
            "rawfile",         "scriptfile",  "luafile",       "stringtable",       "localize",
            "netconststrings", "ttf",         "soundbanklist", "computeshader",     "libshader",
            "vertexshader",    "hullshader",  "domainshader",  "pixelshader",       "image",
            "streamkey",       "xmodelsurfs", "soundbank",     "soundbanktransient"
        };
        Json expected = Json::array();
        uint32_t index = 1;
        for (const auto& pool : profile.at("pools")) {
            const auto id = pool.at("id").get<uint32_t>();
            const auto name = pool.at("name").get<std::string>();
            Put(base + 0x2457A00 + 8 * id, Text(name));
            Put(base + 0x2458160 + 4 * id, pool.at("size").get<uint32_t>());
            const bool schemaFixture = !imageSpec.is_null() && imageSpec.value("schema_fixture", false);
            if (schemaFixture && pool.at("size") == 0)
                continue;
            const bool fieldFixture = !imageSpec.is_null() && imageSpec.value("field_fixture", false);
            if (std::find(pools.begin(), pools.end(), name) == pools.end() && !(fieldFixture && name == "weapon") &&
                !schemaFixture)
                continue;
            if (!imageSpec.is_null() && !schemaFixture && name != imageSpec.value("pool", std::string{ "image" }))
                continue;
            const bool geometryFixture = !imageSpec.is_null() && imageSpec.value("geometry_fixture", false);
            const bool soundFixture = !imageSpec.is_null() && imageSpec.value("sound_fixture", false);
            const auto variants = fieldFixture                               ? 2
                                  : soundFixture                             ? 4
                                  : geometryFixture                          ? 2
                                  : !imageSpec.is_null()                     ? 1
                                  : name == "image"                          ? 4
                                  : name == "rawfile" || name == "streamkey" ? 2
                                                                             : 1;
            for (int variant = 0; variant < variants; variant++) {
                auto header = Buffer(pool.at("size"));
                Put(header, Text("fixture/" + name + "/" + std::to_string(variant)));
                if (fieldFixture && name == "weapon") {
                    const auto first = Buffer(8), second = Buffer(16);
                    Put(header + 548, uint16_t{ 1 });
                    Put(header + 550, uint16_t{ 2 });
                    Put(header + 552, variant ? uint64_t{ 1 } : first);
                    Put(header + 560, second);
                    Put(first, 1.25f);
                    Put(first + 4, 2.5f);
                    for (size_t i = 0; i < 4; ++i)
                        Put(second + 4 * i, static_cast<float>(i + 3));
                } else if (name == "soundbank" || name == "soundbanktransient") {
                    const auto bank = Buffer(1024), key = Buffer(64);
                    Put(header + 496, variant == 3 ? uint64_t{} : key);
                    Put(key, Text("fixture/loaded_sound_bank"));
                    Put(key + 40, bank);
                    Put(key + 56, uint32_t{ 1024 });
                    Put(key + 61, uint8_t{ 2 });
                    Put(bank, 0x23585532u);
                    Put(bank + 4, 10u);
                    Put(bank + 8, 44u);
                    Put(bank + 12, 16u);
                    Put(bank + 16, 64u);
                    Put(bank + 20, 1u);
                    Put(bank + 28, 16u);
                    Put(bank + 32, uint64_t{ 1024 });
                    Put(bank + 40, uint64_t{ 688 });
                    Put(bank + 48, uint64_t{ 752 });
                    Put(bank + 688, 0x1234u);
                    Put(bank + 692, 24u);
                    Put(bank + 696, 4u);
                    Put(bank + 700, 21u);
                    Put(bank + 708, uint64_t{ 800 });
                    Put(bank + 716, 44100u);
                    Put(bank + 720, uint8_t{ 1 });
                    Put(bank + 721, uint8_t{ 1 });
                    Put(bank + 722, static_cast<uint8_t>(variant == 1 ? 9 : 8));
                    // Two independently decodable constant FLAC frames: 16+5 samples of 0x1234.
                    const uint8_t frames[]{ 0xff, 0xf8, 0x69, 0x08, 0, 0x0f, 0x30, 0, 0x12, 0x34, 0xaa, 0x3b,
                                            0xff, 0xf8, 0x69, 0x08, 1, 4,    0x14, 0, 0x12, 0x34, 0x81, 0x7c };
                    std::memcpy(reinterpret_cast<void*>(bank + 804), frames, sizeof(frames));
                    if (soundFixture && variant == 0) {
                        Put(bank + 20, 2u);
                        Put(bank + 48, uint64_t{ 900 });
                        std::memcpy(reinterpret_cast<void*>(bank + 732), reinterpret_cast<void*>(bank + 688), 44);
                        Put(bank + 732, 0x1235u);
                        Put(bank + 766, uint8_t{ 9 }); // --test must not try a second sound.
                    }
                    if (variant == 2)
                        Put(bank + 820, uint8_t{ 0 }); // Break header CRC/frame progression.
                } else if (name == "streamkey" || name == "xmodelsurfs") {
                    const bool model = name == "xmodelsurfs",
                               streamed = !imageSpec.is_null() && !geometryFixture && !schemaFixture;
                    const auto size = geometryFixture ? 66u
                                      : streamed      ? imageSpec.at("total_size").get<uint32_t>()
                                                      : 9u;
                    const auto data = streamed ? uint64_t{ 0xdeadbeef } : Buffer(size);
                    if (!streamed && !geometryFixture) {
                        const uint8_t bytes[]{ 0, 1, 0xff, 0x80, 'M', 'E', 'S', 'H', 0 };
                        std::memcpy(reinterpret_cast<void*>(data), bytes, sizeof(bytes));
                    }
                    Put(header + (model ? 16 : 8),
                        streamed ? imageSpec.at("parts").at(0).at("key").get<uint64_t>() : uint64_t{});
                    if (model) {
                        auto shared = Buffer(16), surface = Buffer(192);
                        Put(shared, data);
                        Put(shared + 8, size);
                        Put(shared + 12, streamed ? 1u : 0u);
                        Put(surface + 72, shared);
                        Put(header + 8, surface);
                        Put(header + 48, shared);
                        Put(header + 56, uint16_t{ 1 });
                        if (geometryFixture) {
                            Put(surface + 2, uint16_t{ 3 });
                            Put(surface + 4, uint16_t{ 1 });
                            Put(surface + 40, uint32_t{ 60 });
                            Put(surface + 48, UINT32_MAX);
                            Put(surface + 52, UINT32_MAX);
                            for (size_t i = 0; i < 3; ++i) {
                                Put(surface + 144 + i * 4, 10.0f * static_cast<float>(i + 1));
                                Put(surface + 156 + i * 4, static_cast<float>(1 << i));
                                Put(data + i * 20 + 16, uint32_t{ 512 | (512 << 10) | (256 << 20) | (3u << 30) });
                                Put(data + 60 + i * 2, static_cast<uint16_t>(i));
                            }
                            Put(data + 20, uint64_t{ 0x1fffff });
                            Put(data + 40, uint64_t{ 0x1fffff } << 21);
                            Put(data + 32, uint16_t{ 0x3c00 });
                            Put(data + 54, uint16_t{ 0x3c00 });
                            if (variant)
                                Put(data + 64, uint16_t{ 3 }); // Deliberately outside the vertex range.
                        }
                    } else {
                        Put(header + 40, data);
                        Put(header + 56, size);
                        Put(header + 61, static_cast<uint8_t>(streamed ? 0 : 2 | variant));
                    }
                } else if (!imageSpec.is_null() && !schemaFixture) {
                    Put(header + 20, imageSpec.at("format").get<uint32_t>());
                    Put(header + 24, uint32_t{ 0x40 });
                    Put(header + 28, imageSpec.at("total_size").get<uint32_t>());
                    Put(header + 36, imageSpec.at("width").get<uint16_t>());
                    Put(header + 38, imageSpec.at("height").get<uint16_t>());
                    Put(header + 40, uint16_t{ 1 });
                    Put(header + 42, uint16_t{ 1 });
                    Put(header + 48, imageSpec.at("mips").get<uint8_t>());
                    Put(header + 49, uint8_t{ 0xfe }); // Not the Replay stream count.
                    const auto& parts = imageSpec.at("parts");
                    if (parts.empty() || parts.size() > 4)
                        throw std::runtime_error("Invalid fixture part count");
                    Put(header + 50, static_cast<uint8_t>(parts.size()));
                    Put(header + 224, uint64_t{ 0xdeadbeef });
                    for (size_t i = 0; i < parts.size(); ++i) {
                        const auto at = header + 56 + 40 * i;
                        Put(at, parts[i].at("key").get<uint64_t>());
                        Put(at + 32, (parts[i].at("size").get<uint32_t>() << 4) | parts[i].at("mips").get<uint32_t>());
                        Put(at + 36, parts[i].at("width").get<uint16_t>());
                        Put(at + 38, parts[i].at("height").get<uint16_t>());
                    }
                } else if (name == "image") {
                    Put(header + 20, uint32_t{ 6 });
                    Put(header + 28, uint32_t{ 16 });
                    Put(header + 36, uint16_t{ 1 });
                    Put(header + 38, uint16_t{ 1 });
                    Put(header + 40, uint16_t{ 1 });
                    Put(header + 42, uint16_t{ 1 });
                    Put(header + 48, uint8_t{ 1 });
                    auto pixels = Buffer(64);
                    std::memcpy(reinterpret_cast<void*>(pixels), "RGBA", 4);
                    Put(header + 224, pixels);
                    if (variant == 1) {
                        Put(header + 24, uint32_t{ 0x20000 });
                        Put(header + 28, uint32_t{ 64 });
                        Put(header + 36, uint16_t{ 2 });
                        Put(header + 42, uint16_t{ 2 });
                        Put(header + 48, uint8_t{ 2 });
                        std::memcpy(reinterpret_cast<void*>(pixels), "AAAAAAAA", 8);
                        std::memcpy(reinterpret_cast<void*>(pixels + 16), "BBBBBBBB", 8);
                        std::memcpy(reinterpret_cast<void*>(pixels + 32), "CCCC", 4);
                        std::memcpy(reinterpret_cast<void*>(pixels + 48), "DDDD", 4);
                    } else if (variant == 2) {
                        Put(header + 224, uint64_t{}); // Native post-upload state.
                    } else if (variant == 3) {
                        Put(header + 8, uint64_t{ 0xdeadbeef });
                        Put(header + 24, uint32_t{ 0x200 });
                        Put(header + 44, uint16_t{ 4 }); // Broken auxiliary atlas, valid DDS pixels.
                    }
                } else if (name == "rawfile") {
                    const std::string text = "fixture payload\n";
                    Put(header + 12, static_cast<uint32_t>(text.size()));
                    if (!variant)
                        Put(header + 16, Text(text));
                    else {
                        uLongf size = compressBound(static_cast<uLong>(text.size()));
                        auto data = Buffer(size);
                        if (compress(
                                reinterpret_cast<Bytef*>(data),
                                &size,
                                reinterpret_cast<const Bytef*>(text.data()),
                                static_cast<uLong>(text.size())
                            ) != Z_OK)
                            throw std::runtime_error("fixture zlib failed");
                        Put(header + 8, static_cast<uint32_t>(size));
                        Put(header + 16, data);
                    }
                } else if (name == "scriptfile") {
                    Put(header + 8, int32_t{ 3 });
                    Put(header + 12, int32_t{ 5 });
                    Put(header + 16, int32_t{ 3 });
                    Put(header + 24, Text("buf"));
                    Put(header + 32, Text("gsc"));
                } else if (name == "luafile" || name == "ttf") {
                    if (name == "ttf")
                        Put(header + 24, uint64_t{ 0xdeadbeef }); // Synthetic FreeType runtime handle: never read.
                    Put(header + 8, int32_t{ 4 });
                    Put(header + 16, Text("DATA"));
                } else if (name == "localize") {
                    Put(header + 8, Text("Fixture translation"));
                } else if (name == "stringtable") {
                    Put(header + 8, int32_t{ 2 });
                    Put(header + 12, int32_t{ 1 });
                    Put(header + 16, int32_t{ 2 });
                    auto indices = Buffer(4);
                    Put(indices, uint16_t{ 1 });
                    Put(indices + 2, uint16_t{ 0 });
                    Put(header + 24, indices);
                    Put(header + 32, Buffer(8));
                    auto strings = Buffer(16);
                    Put(strings, Text("quoted \"cell\""));
                    Put(strings + 8, Text("first"));
                    Put(header + 40, strings);
                } else if (name == "netconststrings") {
                    Put(header + 20, uint32_t{ 2 });
                    auto strings = Buffer(16);
                    Put(strings, Text("first"));
                    Put(strings + 8, Text("second"));
                    Put(header + 24, strings);
                } else if (name == "soundbanklist") {
                    Put(header + 8, uint16_t{ 2 });
                    auto hashes = Buffer(8);
                    Put(hashes, uint32_t{ 0x12345678 });
                    Put(hashes + 4, uint32_t{ 0x89abcdef });
                    Put(header + 16, hashes);
                } else if (schemaFixture && !name.ends_with("shader")) {
                    // Named zero root: breadth smoke test only.
                } else {
                    Put(header + 24, Text("DXBCfixture"));
                    Put(header + 32, uint32_t{ 11 });
                }
                auto entry = entries + index * 20;
                Put(entry, header);
                Put(entry + 16, uint8_t{ 1 });
                Put(entry + 17, static_cast<uint8_t>(id));
                auto flag = reinterpret_cast<uint64_t*>(flags + (index / 64) * 8);
                *flag |= uint64_t{ 1 } << (63 - index % 64);
                expected.push_back(Json{ { "entry_index", index }, { "pool", name } });
                index++;
            }
        }
        std::ofstream ready(argv[2]);
        ready << Json{ { "pid", GetCurrentProcessId() }, { "assets", expected } }.dump(2);
        ready.close();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
        while (!std::filesystem::exists(argv[3]) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
