#ifndef MW19_SCHEMA_STANDALONE
#include <includes.hpp>
#endif
#include "mw19_linker.hpp"
#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>
#include <zlib.h>
#include <lz4.h>

namespace tool::mw19::linker {
    namespace {
        constexpr uint64_t Follows = UINT64_MAX - 1, Insert = UINT64_MAX - 2;
        uint32_t Number(const Json& value, uint32_t maximum = UINT32_MAX) {
            if (!value.is_number_integer() || (!value.is_number_unsigned() && value.get<int64_t>() < 0))
                throw std::runtime_error("Linker field requires a nonnegative integer");
            const auto n = value.get<uint64_t>();
            if (n > maximum)
                throw std::runtime_error("Linker integer is out of range");
            return static_cast<uint32_t>(n);
        }
        uint32_t CellHash(const std::string& value) {
            // IW8 StringTable uses Com_HashStringLower (ASCII case folding).
            uint32_t hash = 0;
            for (unsigned char c : value) {
                if (c >= 'A' && c <= 'Z')
                    c += 'a' - 'A';
                hash = hash * 31 + c;
            }
            return hash;
        }
        template<typename T>
        void Put(std::vector<uint8_t>& data, size_t at, T value) {
            if (at > data.size() || sizeof(value) > data.size() - at)
                throw std::runtime_error("Link field outside record");
            std::memcpy(data.data() + at, &value, sizeof(value));
        }
        std::string String(const Json& value) {
            auto s = value.get<std::string>();
            if (s.size() > 65535 || s.find('\0') != std::string::npos)
                throw std::runtime_error("Invalid asset string");
            return s;
        }
        std::vector<uint8_t> Bytes(const Json& asset, const char* field, size_t limit) {
            if (!asset.contains(field))
                return {};
            const auto& value = asset.at(field);
            if (value.is_binary()) {
                const auto& b = value.get_binary();
                if (b.size() > limit)
                    throw std::runtime_error("Asset bytes exceed budget");
                return { b.begin(), b.end() };
            }
            auto s = value.get<std::string>();
            if (s.size() > limit)
                throw std::runtime_error("Asset bytes exceed budget");
            return { s.begin(), s.end() };
        }
        std::vector<uint8_t> Deflate(const std::vector<uint8_t>& input) {
            uLongf count = compressBound(static_cast<uLong>(input.size()));
            std::vector<uint8_t> out(count);
            if (compress2(out.data(), &count, input.data(), static_cast<uLong>(input.size()), Z_BEST_COMPRESSION) !=
                Z_OK)
                throw std::runtime_error("Asset zlib compression failed");
            out.resize(count);
            return out;
        }
        class Writer {
            size_t limit;

          public:
            Zone zone;
            explicit Writer(size_t maximum) : limit(maximum) {}
            void Align(size_t block, size_t alignment) {
                auto& n = zone.blocks.at(block);
                n = (n + alignment - 1) & ~(alignment - 1);
                if (n > limit)
                    throw std::runtime_error("Stream reservation exceeds budget");
            }
            void Reserve(size_t block, size_t size) {
                auto& n = zone.blocks.at(block);
                if (size > limit - n)
                    throw std::runtime_error("Stream reservation exceeds budget");
                n += size;
            }
            void Write(size_t block, const std::vector<uint8_t>& bytes) {
                if (bytes.size() > limit - zone.body.size())
                    throw std::runtime_error("Zone exceeds budget");
                Reserve(block, bytes.size());
                zone.body.insert(zone.body.end(), bytes.begin(), bytes.end());
            }
            void Text(size_t block, const std::string& text) {
                std::vector<uint8_t> bytes(text.begin(), text.end());
                bytes.push_back(0);
                Write(block, bytes);
            }
        };
    } // namespace
    Zone Link(const Json& profile, const Json& assets, size_t limit) {
        if (profile.at("id") != "replay-1.20" || !assets.is_array() || assets.size() > 65535 ||
            limit > 128 * 1024 * 1024)
            throw std::runtime_error("Invalid Replay zone profile, asset count or budget");
        Writer w(limit);
        std::vector<const Json*> pools;
        std::set<std::pair<uint32_t, std::string>> names;
        for (const auto& a : assets) {
            const auto name = String(a.at("name"));
            if (name.empty() || name[0] == ',')
                throw std::runtime_error("Concrete asset name required");
            const Json* pool = nullptr;
            for (const auto& p : profile.at("pools"))
                if (p.at("name") == a.at("pool")) {
                    pool = &p;
                    break;
                }
            static const std::set<std::string> supported{ "rawfile",       "luafile",    "ttf",
                                                          "localize",      "scriptfile", "netconststrings",
                                                          "soundbanklist", "stringtable" };
            if (!pool || !supported.contains(a.at("pool").get<std::string>()))
                throw std::runtime_error("No Replay linker for asset pool: " + a.at("pool").get<std::string>());
            if (!names.emplace(pool->at("id").get<uint32_t>(), name).second)
                throw std::runtime_error("Duplicate zone asset");
            pools.push_back(pool);
        }
        std::vector<uint8_t> root(32);
        Put(root, 16, static_cast<uint32_t>(assets.size()));
        Put(root, 24, assets.empty() ? uint64_t{ 0 } : Follows);
        w.Write(0, root);
        w.Align(5, 8);
        std::vector<uint8_t> table(assets.size() * 16);
        for (size_t i = 0; i < assets.size(); ++i) {
            Put(table, i * 16, pools[i]->at("id").get<uint32_t>());
            Put(table, i * 16 + 8, Insert);
        }
        w.Write(5, table);
        for (size_t i = 0; i < assets.size(); ++i) {
            const auto& a = assets[i];
            const auto& pool = *pools[i];
            const auto type = a.at("pool").get<std::string>(), name = String(a.at("name"));
            std::vector<uint8_t> header(pool.at("size").get<size_t>());
            Put(header, 0, Follows);
            std::vector<uint8_t> data, bytecode, indices, hashes;
            std::vector<std::string> strings;
            if (type == "rawfile") {
                auto raw = Bytes(a, "data", limit);
                data = Deflate(raw);
                Put(header, 8, static_cast<uint32_t>(data.size()));
                Put(header, 12, static_cast<uint32_t>(raw.size()));
                Put(header, 16, Follows);
            } else if (type == "luafile" || type == "ttf") {
                data = Bytes(a, "data", limit);
                Put(header, 8, static_cast<uint32_t>(data.size()));
                Put(header, 16, Follows);
                if (type == "ttf")
                    data.push_back(0);
                else
                    Put(header, 12, static_cast<uint8_t>(Number(a.value("stripping_type", Json(0)), 255)));
            } else if (type == "localize") {
                strings.push_back(String(a.at("value")));
                Put(header, 8, Follows);
            } else if (type == "scriptfile") {
                auto stack = Bytes(a, "stack", limit);
                data = Deflate(stack);
                bytecode = Bytes(a, "bytecode", limit);
                Put(header, 8, static_cast<uint32_t>(data.size()));
                Put(header, 12, static_cast<uint32_t>(stack.size()));
                Put(header, 16, static_cast<uint32_t>(bytecode.size()));
                Put(header, 24, Follows);
                Put(header, 32, bytecode.empty() ? uint64_t{ 0 } : Follows);
            } else if (type == "netconststrings") {
                const auto& entries = a.at("strings");
                if (!entries.is_array() || entries.size() > 65535)
                    throw std::runtime_error("Invalid netconststrings count");
                for (const auto& s : entries)
                    strings.push_back(String(s));
                Put(header, 8, Number(a.at("string_type")));
                Put(header, 12, Number(a.at("source_type")));
                Put(header, 16, Number(a.value("flags", Json(0))));
                Put(header, 20, static_cast<uint32_t>(strings.size()));
                Put(header, 24, strings.empty() ? uint64_t{ 0 } : Follows);
            } else if (type == "soundbanklist") {
                const auto& entries = a.at("hashes");
                if (!entries.is_array() || entries.size() > 65535)
                    throw std::runtime_error("Invalid sound bank count");
                data.resize(entries.size() * 4);
                for (size_t j = 0; j < entries.size(); ++j) {
                    Put(data, j * 4, Number(entries[j]));
                }
                Put(header, 8, static_cast<uint16_t>(entries.size()));
                Put(header, 16, data.empty() ? uint64_t{ 0 } : Follows);
            } else if (type == "stringtable") {
                const auto& rows = a.at("rows");
                if (!rows.is_array() || rows.size() > 65535)
                    throw std::runtime_error("Invalid stringtable rows");
                const size_t cols = rows.empty() ? 0 : rows[0].size();
                if (cols > 65535 || rows.size() * cols > 65535)
                    throw std::runtime_error("Stringtable exceeds cell budget");
                indices.resize(rows.size() * cols * 2);
                std::map<std::string, uint16_t> dict;
                for (size_t row = 0; row < rows.size(); ++row) {
                    if (!rows[row].is_array() || rows[row].size() != cols)
                        throw std::runtime_error("Ragged stringtable");
                    for (size_t col = 0; col < cols; ++col) {
                        auto s = String(rows[row][col]);
                        auto [it, added] = dict.emplace(s, static_cast<uint16_t>(strings.size()));
                        if (added)
                            strings.push_back(s);
                        Put(indices, (col * rows.size() + row) * 2, it->second);
                    }
                }
                const auto supplied = a.value("hashes", Json());
                if (!supplied.is_null() && (!supplied.is_array() || supplied.size() != strings.size()))
                    throw std::runtime_error("Stringtable needs one hash per unique cell in first-seen row order");
                hashes.resize(strings.size() * 4);
                for (size_t j = 0; j < strings.size(); ++j) {
                    Put(hashes, j * 4, supplied.is_null() ? CellHash(strings[j]) : Number(supplied[j]));
                }
                Put(header, 8, static_cast<uint32_t>(cols));
                Put(header, 12, static_cast<uint32_t>(rows.size()));
                Put(header, 16, static_cast<uint32_t>(strings.size()));
                if (!indices.empty()) {
                    Put(header, 24, Follows);
                    Put(header, 32, Follows);
                    Put(header, 40, Follows);
                }
            }
            const auto bodyStart = w.zone.body.size();
            w.Align(1, 8);
            w.Align(5, 8);
            w.Reserve(5, 8); // DB_InsertPointer consumes memory only.
            w.Write(1, header);
            w.Text(5, name);
            if (type == "localize")
                w.Text(5, strings[0]);
            else if (type == "netconststrings" || type == "stringtable") {
                if (!indices.empty()) {
                    w.Align(5, 2);
                    w.Write(5, indices);
                    w.Align(5, 4);
                    w.Write(5, hashes);
                }
                if (!strings.empty()) {
                    w.Align(5, 8);
                    std::vector<uint8_t> ptrs(strings.size() * 8);
                    for (size_t j = 0; j < strings.size(); ++j)
                        Put(ptrs, j * 8, Follows);
                    w.Write(5, ptrs);
                    for (const auto& s : strings)
                        w.Text(5, s);
                }
            } else if (type == "scriptfile") {
                w.Write(6, data);
                w.Write(6, bytecode);
            } else {
                if (type == "luafile")
                    w.Align(5, 16);
                if (type == "soundbanklist")
                    w.Align(5, 4);
                w.Write(5, data);
            }
            w.zone.assets.push_back(
                Json{ { "pool", type },
                      { "name", name },
                      { "body_offset", bodyStart },
                      { "body_bytes", w.zone.body.size() - bodyStart } }
            );
        }
        return std::move(w.zone);
    }
    std::vector<uint8_t> Pack(const Zone& zone, bool lz4, size_t chunkSize) {
        if (zone.body.empty() || zone.body.size() > 128 * 1024 * 1024 || !chunkSize || chunkSize > 0x10000)
            throw std::runtime_error("Invalid zone size or compression chunk size");
        std::vector<uint8_t> output(0x94);
        std::memcpy(output.data(), "IWffc100", 8);
        Put(output, 8, uint32_t{ 11 });
        Put(output, 12, uint32_t{ 0xff7 });
        output[16] = lz4 ? 1 : 0;
        Put(output, 28, static_cast<uint32_t>(zone.body.size()));
        Put(output, 32, static_cast<uint64_t>(zone.body.size()));
        for (size_t i = 0; i < 11; ++i)
            Put(output, 48 + i * 8, zone.blocks[i]);
        Put(output, 0x88, uint32_t{ 0x43574902 });
        Put(output, 0x8c, static_cast<uint32_t>(zone.body.size()));
        Put(output, 0x90, static_cast<uint32_t>(chunkSize) | (uint32_t{ lz4 ? 5u : 1u } << 24));
        for (size_t at = 0; at < zone.body.size(); at += chunkSize) {
            size_t count = std::min(chunkSize, zone.body.size() - at);
            std::vector<uint8_t> block;
            if (lz4) {
                block.resize(LZ4_compressBound(static_cast<int>(count)));
                auto n = LZ4_compress_default(
                    reinterpret_cast<const char*>(zone.body.data() + at),
                    reinterpret_cast<char*>(block.data()),
                    static_cast<int>(count),
                    static_cast<int>(block.size())
                );
                if (n <= 0)
                    throw std::runtime_error("Fastfile LZ4 compression failed");
                block.resize(n);
            } else
                block.assign(zone.body.begin() + at, zone.body.begin() + at + count);
            auto offset = output.size();
            output.resize(offset + 12);
            Put(output, offset, static_cast<uint32_t>(block.size()));
            Put(output, offset + 4, static_cast<uint32_t>(count));
            output.insert(output.end(), block.begin(), block.end());
            output.resize((output.size() + 3) & ~size_t{ 3 });
        }
        Put(output, 20, static_cast<uint32_t>(output.size() - 0x88));
        return output;
    }
} // namespace tool::mw19::linker
