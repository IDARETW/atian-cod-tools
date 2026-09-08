#ifndef MW19_SCHEMA_STANDALONE
#include <includes.hpp>
#endif
#include "mw19_schema.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cctype>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#if defined(_WIN32) && defined(MW19_SCHEMA_STANDALONE)
#define NOMINMAX
#include <Windows.h>
#endif

namespace tool::mw19::schema {
    namespace {
        uint64_t Add(uint64_t a, uint64_t b) {
            if (b > std::numeric_limits<uint64_t>::max() - a)
                throw std::runtime_error("address overflow");
            return a + b;
        }
        std::string Hex(uint64_t n) {
            std::ostringstream out;
            out << "0x" << std::hex << n;
            return out.str();
        }
        std::string HexBytes(const std::vector<uint8_t>& data) {
            constexpr char hex[] = "0123456789abcdef";
            std::string out(data.size() * 2, '0');
            for (size_t i = 0; i < data.size(); i++) {
                out[i * 2] = hex[data[i] >> 4];
                out[i * 2 + 1] = hex[data[i] & 15];
            }
            return out;
        }
        void StoreString(Json& result, const std::string& key, const std::string& value) {
            try {
                (void)Json(value).dump();
                result[key] = value;
            } catch (const Json::type_error&) {
                result[key] = nullptr;
                result[key + "_encoding"] = "opaque_bytes";
                result[key + "_bytes"] = HexBytes({ value.begin(), value.end() });
            }
        }
        template<typename T>
        T ReadExact(const MemoryReader& read, uint64_t address) {
            T result{};
            if (!read(address, &result, sizeof(result)))
                throw std::runtime_error("unreadable address " + Hex(address));
            return result;
        }
    } // namespace

    Database::Database(const std::filesystem::path& path) {
        std::ifstream in(path);
        if (!in)
            throw std::runtime_error("Cannot open MW2019 schema: " + path.string());
        in >> document;
        Validate();
    }
    Database::Database(Json value) : document(std::move(value)) { Validate(); }
    const Json& Database::Profile(const std::string& id) const { return document.at("profiles").at(id); }
    const Json& Database::Type(const std::string& name) const { return document.at("types").at(name); }
    const Json& Database::Pool(const Json& profile, const std::string& name) const {
        for (const auto& pool : profile.at("pools")) {
            if (pool.at("name") == name || std::to_string(pool.at("id").get<unsigned>()) == name)
                return pool;
        }
        throw std::runtime_error("Unknown MW2019 pool: " + name);
    }
    void Database::Validate() const {
        if (document.at("schema_version") != 1)
            throw std::runtime_error("Unsupported MW2019 schema version");
        for (const auto& profile : document.at("profiles")) {
            if (profile.at("pools").size() != profile.at("pool_count"))
                throw std::runtime_error("Pool count mismatch");
            size_t expected{};
            std::set<std::string> names;
            for (const auto& p : profile.at("pools")) {
                if (p.at("id") != expected++)
                    throw std::runtime_error("Noncontiguous pool ids");
                if (!names.insert(p.at("name")).second)
                    throw std::runtime_error("Duplicate pool name");
                if (!p.at("root_type").is_null()) {
                    auto root = p.at("root_type").get<std::string>();
                    if (profile.contains("type_overrides") && profile.at("type_overrides").contains(root))
                        root = profile.at("type_overrides").at(root);
                    if (Type(root).at("size") != p.at("size"))
                        throw std::runtime_error("Pool root size mismatch: " + p.at("name").get<std::string>());
                }
            }
        }
        for (const auto& type : document.at("types")) {
            const auto kind = type.at("kind").get<std::string>();
            if (kind == "pointer")
                Type(type.at("target"));
            if (kind == "array")
                Type(type.at("element"));
            if (type.contains("members")) {
                for (const auto& member : type.at("members")) {
                    Type(member.at("type"));
                    const auto offset = member.at("offset_bits").get<uint64_t>();
                    const auto bits = member.at("size_bits").get<uint64_t>();
                    const auto total = type.at("size").get<uint64_t>() * 8;
                    if (offset > total || bits > total - offset)
                        throw std::runtime_error("Member outside type: " + type.at("name").get<std::string>());
                }
            }
        }
        if (document.contains("pointer_rules")) {
            std::function<void(const Json&, const Json&)> validateRule = [&](const Json& type, const Json& rule) {
                if (rule.is_null())
                    return; // Unresolved slots must remain visible at traversal time.
                if (type.at("kind") == "pointer") {
                    if (!rule.is_object() || rule.contains("count") == rule.contains("runtime") ||
                        !rule.contains("evidence") || rule.contains("elements"))
                        throw std::runtime_error("Invalid pointer extent rule");
                    if (rule.contains("runtime") &&
                        (!rule.at("runtime").is_string() || rule.at("runtime").get<std::string>().empty() ||
                         rule.contains("pointee")))
                        throw std::runtime_error("Invalid runtime reference rule");
                    if (rule.contains("pointee"))
                        validateRule(Type(type.at("target")), rule.at("pointee"));
                } else if (type.at("kind") == "array") {
                    if (!rule.is_object() || !rule.contains("elements") || !rule.at("elements").is_array() ||
                        rule.at("elements").size() != type.at("count"))
                        throw std::runtime_error("Invalid pointer array rule");
                    for (const auto& element : rule.at("elements"))
                        validateRule(Type(type.at("element")), element);
                } else
                    throw std::runtime_error("Pointer rule applied to non-pointer member");
            };
            for (const auto& [name, rules] : document.at("pointer_rules").items()) {
                const auto& members = Type(name).at("members");
                for (const auto& [field, rule] : rules.items()) {
                    const auto member = std::find_if(members.begin(), members.end(), [&](const Json& m) {
                        return m.at("name") == field;
                    });
                    if (member == members.end())
                        throw std::runtime_error("Unknown pointer rule member: " + name + "." + field);
                    validateRule(Type(member->at("type")), rule);
                }
            }
        }
        if (document.contains("union_rules")) {
            for (const auto& [name, rule] : document.at("union_rules").items()) {
                const auto& type = Type(name);
                if (type.at("kind") != "union" || !rule.contains("selector") || !rule.contains("evidence"))
                    throw std::runtime_error("Invalid union rule: " + name);
                std::set<uint64_t> selectors;
                for (const auto& choice : rule.at("cases")) {
                    if (!selectors.insert(choice.at("value").get<uint64_t>()).second)
                        throw std::runtime_error("Duplicate union selector: " + name);
                    const auto& members = type.at("members");
                    const auto member = std::find_if(members.begin(), members.end(), [&](const Json& m) {
                        return m.at("name") == choice.at("member");
                    });
                    if (member == members.end())
                        throw std::runtime_error("Unknown union member: " + name);
                    const auto& selected = Type(member->at("type"));
                    const auto kind = selected.at("kind").get<std::string>();
                    if ((choice.contains("count") && kind != "pointer") ||
                        (choice.contains("inline_count") && kind != "array") ||
                        (choice.value("external", false) && kind == "pointer"))
                        throw std::runtime_error("Invalid union extent or external handle: " + name);
                    if (choice.value("string", false) && (kind != "pointer" || (selected.at("target") != "char" &&
                                                                                selected.at("target") != "const char")))
                        throw std::runtime_error("Invalid union string member: " + name);
                    if (choice.contains("element_count") &&
                        (kind != "array" || Type(selected.at("element")).at("kind") != "pointer"))
                        throw std::runtime_error("Invalid union pointer array: " + name);
                    if (choice.contains("runtime") && (kind != "pointer" || !choice.at("runtime").is_string() ||
                                                       choice.contains("count") || choice.value("string", false)))
                        throw std::runtime_error("Invalid runtime union member: " + name);
                }
            }
        }
    }

    std::string ReadString(const MemoryReader& read, uint64_t address, size_t limit) {
        if (!address)
            throw std::runtime_error("null string");
        std::string result;
        for (size_t i = 0; i < limit; i++) {
            const auto c = ReadExact<char>(read, Add(address, i));
            if (!c)
                return result;
            result.push_back(c);
        }
        throw std::runtime_error("unterminated string at " + Hex(address));
    }
    void ValidateProfile(const MemoryReader& read, uint64_t base, const Json& profile) {
        const auto& tables = profile.at("table_rvas");
        for (const auto& p : profile.at("pools")) {
            const auto id = p.at("id").get<uint64_t>();
            const auto size = ReadExact<uint32_t>(read, Add(base, tables.at("sizes").get<uint64_t>() + id * 4));
            const auto namePtr = ReadExact<uint64_t>(read, Add(base, tables.at("names").get<uint64_t>() + id * 8));
            if (size != p.at("size") || ReadString(read, namePtr, 128) != p.at("name").get<std::string>()) {
                throw std::runtime_error("Executable does not match profile at pool " + std::to_string(id));
            }
        }
    }
    std::vector<AssetEntry> ReadReplayEntries(const MemoryReader& read, uint64_t table, const Json& profile) {
        const auto count = profile.at("entry_capacity").get<size_t>();
        const auto flagsOffset = profile.at("allocation_flags_offset").get<uint64_t>();
        if (!table || count > 1000000 || profile.at("entry_size") != 20 || count % 64)
            throw std::runtime_error("Invalid entry pool metadata");
        std::vector<uint64_t> before(count / 64), after(count / 64);
        std::vector<uint8_t> records(count * 20);
        if (!read(Add(table, flagsOffset), before.data(), before.size() * 8) ||
            !read(table, records.data(), records.size()) ||
            !read(Add(table, flagsOffset), after.data(), after.size() * 8))
            throw std::runtime_error("Cannot read entry pool snapshot");
        if (before != after)
            throw std::runtime_error("Asset allocation changed during snapshot; retry after zone loading finishes");
        std::vector<AssetEntry> result;
        for (size_t i = 0; i < count; i++) {
            // IW8 allocation flags use the most significant bit first.
            if (!(before[i / 64] & (uint64_t{ 1 } << (63 - i % 64))))
                continue;
            AssetEntry e{};
            const auto* p = records.data() + i * 20;
            std::memcpy(&e.header, p, 8);
            std::memcpy(&e.nextHash, p + 8, 4);
            std::memcpy(&e.nextStashed, p + 12, 4);
            e.inUse = p[16];
            e.type = p[17];
            std::memcpy(&e.zoneFlags, p + 18, 2);
            e.index = static_cast<uint32_t>(i);
            if (!e.header || e.type >= profile.at("pool_count").get<size_t>() || e.nextHash >= count ||
                e.nextStashed >= count) {
                throw std::runtime_error("Invalid allocated asset entry " + std::to_string(i));
            }
            result.push_back(e);
        }
        return result;
    }
    std::filesystem::path AssetPath(const std::filesystem::path& root, std::string name) {
        std::replace(name.begin(), name.end(), '\\', '/');
        if (name.empty() || name.front() == '/' || name.find(':') != std::string::npos)
            throw std::runtime_error("Invalid asset path");
        std::filesystem::path rel{ name };
        for (const auto& part : rel) {
            const auto s = part.string();
            auto stem = s.substr(0, s.find('.'));
            std::transform(stem.begin(), stem.end(), stem.begin(), [](unsigned char c) {
                return static_cast<char>(std::toupper(c));
            });
            const bool device = stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL" ||
                                (stem.size() == 4 && (stem.starts_with("COM") || stem.starts_with("LPT")) &&
                                 stem[3] >= '1' && stem[3] <= '9');
            if (s == ".." || s == "." || s.empty() || s.back() == '.' || s.back() == ' ' || device ||
                s.find_first_of("<>\"|?*") != std::string::npos ||
                std::any_of(s.begin(), s.end(), [](unsigned char c) { return c < 32; })) {
                throw std::runtime_error("Unsafe asset path: " + name);
            }
        }
        return root / rel;
    }
    void WriteJsonAtomic(const std::filesystem::path& path, const Json& value) {
        std::filesystem::create_directories(path.parent_path());
        auto temp = path;
        temp += ".tmp";
        {
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            if (!out)
                throw std::runtime_error("Cannot write " + temp.string());
            out << value.dump(2, ' ', false, Json::error_handler_t::replace) << '\n';
            out.flush();
            if (!out)
                throw std::runtime_error("Write failed: " + temp.string());
        }
#ifdef _WIN32
        // Windows rename cannot replace an existing destination. Use the native
        // replace operation so a failed write never destroys the previous result.
        if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Atomic replace failed: " + path.string());
#else
        std::filesystem::rename(temp, path);
#endif
    }

    Inspector::Inspector(const Database& db, MemoryReader reader, Limits lim, std::string profile)
        : database(db), read(std::move(reader)), limits(lim), profileId(std::move(profile)) {}
    const Json& Inspector::Type(const std::string& name) const {
        const auto& profile = database.Profile(profileId);
        if (profile.contains("type_overrides") && profile.at("type_overrides").contains(name)) {
            return database.Type(profile.at("type_overrides").at(name));
        }
        return database.Type(name);
    }
    uint64_t Inspector::Count(const Json& expr, size_t depth) {
        if (depth > 32)
            throw std::runtime_error("Count expression depth exceeded");
        if (expr.is_number_unsigned() || (expr.is_number_integer() && expr.get<int64_t>() >= 0))
            return expr.get<uint64_t>();
        if (expr.contains("context_exists")) {
            const auto owner = expr.at("context_exists").get<std::string>();
            return std::any_of(contexts.begin(), contexts.end(), [&](const auto& item) { return item.first == owner; });
        }
        if (expr.contains("indexed_count")) {
            const auto& lookup = expr.at("indexed_count");
            const auto owner = lookup.at("owner").get<std::string>();
            const auto itemType = lookup.at("item_type").get<std::string>();
            auto contextAddress = [&](const std::string& name) {
                const auto context = std::find_if(contexts.rbegin(), contexts.rend(), [&](const auto& item) {
                    return item.first == name;
                });
                if (context == contexts.rend())
                    throw std::runtime_error("Missing indexed count context: " + name);
                return context->second;
            };
            const auto ownerAddress = contextAddress(owner), itemAddress = contextAddress(itemType);
            auto member = [&](const Json& name) -> const Json& {
                const auto& members = Type(owner).at("members");
                const auto found =
                    std::find_if(members.begin(), members.end(), [&](const Json& m) { return m.at("name") == name; });
                if (found == members.end() || found->at("offset_bits").get<uint64_t>() % 8)
                    throw std::runtime_error("Invalid indexed count member");
                return *found;
            };
            const auto& items = member(lookup.at("items"));
            const auto& counts = member(lookup.at("counts"));
            const auto& itemsPointer = Type(items.at("type"));
            const auto& countsPointer = Type(counts.at("type"));
            if (itemsPointer.at("kind") != "pointer" || itemsPointer.at("target") != itemType ||
                countsPointer.at("kind") != "pointer")
                throw std::runtime_error("Invalid indexed count pointer type");
            const auto array = Unsigned(Add(ownerAddress, items.at("offset_bits").get<uint64_t>() / 8), 8);
            const auto stride = Type(itemType).at("size").get<uint64_t>();
            const auto length = Count(lookup.at("limit"), depth + 1);
            if (!array || !stride || itemAddress < array || (itemAddress - array) % stride ||
                length > limits.maxArray || (itemAddress - array) / stride >= length)
                throw std::runtime_error("Indexed count item outside array");
            const auto index = (itemAddress - array) / stride;
            const Json* element = &Type(countsPointer.at("target"));
            const auto countStride = element->at("size").get<uint64_t>();
            uint64_t fieldOffset{};
            if (lookup.contains("count_path")) {
                for (const auto& field : lookup.at("count_path")) {
                    if (element->at("kind") != "struct")
                        throw std::runtime_error("Indexed count path requires a struct");
                    const auto& fields = element->at("members");
                    const auto found = std::find_if(fields.begin(), fields.end(), [&](const Json& m) {
                        return m.at("name") == field;
                    });
                    if (found == fields.end() || found->at("offset_bits").get<uint64_t>() % 8)
                        throw std::runtime_error("Invalid indexed count field");
                    fieldOffset = Add(fieldOffset, found->at("offset_bits").get<uint64_t>() / 8);
                    element = &Type(found->at("type"));
                }
            }
            const auto width = element->at("size").get<uint64_t>();
            if (element->at("kind") != "scalar" || !width || width > 8 || fieldOffset > countStride ||
                width > countStride - fieldOffset)
                throw std::runtime_error("Indexed count element is not an integer");
            const auto countArray = Unsigned(Add(ownerAddress, counts.at("offset_bits").get<uint64_t>() / 8), 8);
            if (!countArray)
                throw std::runtime_error("Missing indexed count array");
            if (countStride && index > UINT64_MAX / countStride)
                throw std::runtime_error("Indexed count offset overflow");
            return Unsigned(Add(Add(countArray, index * countStride), fieldOffset), width);
        }
        if (expr.contains("owner")) {
            const auto owner = expr.at("owner").get<std::string>();
            auto context =
                std::find_if(contexts.rbegin(), contexts.rend(), [&](const auto& item) { return item.first == owner; });
            if (context == contexts.rend())
                throw std::runtime_error("Missing count context: " + owner);
            uint64_t at = context->second;
            const Json* type = &Type(owner);
            for (const auto& part : expr.at("path")) {
                if (part.is_number_unsigned() || part.is_number_integer()) {
                    auto index = part.get<size_t>();
                    if (type->at("kind") != "array" || index >= type->at("count").get<size_t>())
                        throw std::runtime_error("Invalid count member index");
                    type = &Type(type->at("element"));
                    at = Add(at, index * type->at("size").get<uint64_t>());
                } else {
                    const auto& members = type->at("members");
                    auto member = std::find_if(members.begin(), members.end(), [&](const Json& value) {
                        return value.at("name") == part;
                    });
                    if (member == members.end() || member->at("offset_bits").get<size_t>() % 8)
                        throw std::runtime_error("Invalid count member");
                    at = Add(at, member->at("offset_bits").get<uint64_t>() / 8);
                    type = &Type(member->at("type"));
                }
            }
            if (type->at("kind") != "scalar" && type->at("kind") != "enum")
                throw std::runtime_error("Non-scalar count member");
            return Unsigned(at, type->at("size"));
        }
        const auto& args = expr.at("args");
        const auto op = expr.at("op").get<std::string>();
        if (op == "popcount") {
            if (args.size() != 1)
                throw std::runtime_error("Invalid population count operands");
            return std::popcount(Count(args[0], depth + 1));
        }
        if (op == "select") {
            if (args.size() != 3)
                throw std::runtime_error("Invalid conditional expression operands");
            return Count(args[Count(args[0], depth + 1) ? 1 : 2], depth + 1);
        }
        if (args.size() != 2)
            throw std::runtime_error("Invalid count expression operands");
        const auto a = Count(args[0], depth + 1), b = Count(args[1], depth + 1);
        if (op == "choose_nonzero")
            return a ? a : b;
        if (op == "eq")
            return a == b;
        if (op == "ne")
            return a != b;
        if (op == "ge")
            return a >= b;
        if (op == "gt")
            return a > b;
        if (op == "add")
            return Add(a, b);
        if (op == "sub") {
            if (b > a)
                throw std::runtime_error("Negative array count");
            return a - b;
        }
        if (op == "mul") {
            if (b && a > UINT64_MAX / b)
                throw std::runtime_error("Array count overflow");
            return a * b;
        }
        if (op == "div") {
            if (!b || a % b)
                throw std::runtime_error("Nonintegral array extent");
            return a / b;
        }
        if (op == "and")
            return a & b;
        if (op == "or")
            return a | b;
        if (op == "shr") {
            if (b >= 64)
                throw std::runtime_error("Invalid count shift");
            return a >> b;
        }
        if (op == "shl") {
            if (b >= 64 || a > (UINT64_MAX >> b))
                throw std::runtime_error("Count shift overflow");
            return a << b;
        }
        throw std::runtime_error("Unknown count expression operation");
    }
    bool Inspector::ContainsPointers(const std::string& name) {
        if (auto cached = pointerTypes.find(name); cached != pointerTypes.end())
            return cached->second;
        const auto& type = Type(name);
        const auto kind = type.at("kind").get<std::string>();
        pointerTypes[name] = false;
        bool pointers = kind == "pointer";
        if (kind == "array")
            pointers = ContainsPointers(type.at("element"));
        else if (kind == "struct" || kind == "union") {
            for (const auto& member : type.at("members")) {
                if (ContainsPointers(member.at("type"))) {
                    pointers = true;
                    break;
                }
            }
        }
        pointerTypes[name] = pointers;
        return pointers;
    }
    Json Inspector::Union(const std::string& name, const Json& type, uint64_t address, size_t depth) {
        const auto bytes = Read(address, type.at("size"));
        Json result{ { "bytes", HexBytes(bytes) } };
        if (database.document.contains("union_rules") && database.document.at("union_rules").contains(name)) {
            const auto& rule = database.document.at("union_rules").at(name);
            const auto selector = Count(rule.at("selector"));
            result["selector"] = selector;
            result["selector_evidence"] = rule.at("evidence");
            for (const auto& choice : rule.at("cases")) {
                if (choice.at("value") != selector)
                    continue;
                const auto memberName = choice.at("member").get<std::string>();
                const auto& members = type.at("members");
                const auto member = std::find_if(members.begin(), members.end(), [&](const Json& m) {
                    return m.at("name") == memberName;
                });
                if (member == members.end())
                    throw std::runtime_error("Missing union member: " + memberName);
                const auto memberType = member->at("type").get<std::string>();
                const auto at = Add(address, member->at("offset_bits").get<uint64_t>() / 8);
                result["active_member"] = memberName;
                if (Type(memberType).at("kind") == "pointer") {
                    Json extent{ { "count", choice.value("count", Json(1)) }, { "evidence", rule.at("evidence") } };
                    if (choice.contains("runtime")) {
                        extent.erase("count");
                        extent["runtime"] = choice.at("runtime");
                    }
                    result["value"] =
                        Pointer(Type(memberType), at, depth + 1, choice.value("string", false) ? nullptr : &extent);
                    if (choice.value("required_if_nonempty", false) && result["value"].is_null() &&
                        Count(extent.at("count"))) {
                        result["status"] = "resident_payload_unavailable";
                        external++;
                    }
                } else if (choice.contains("element_count")) {
                    const auto& array = Type(memberType);
                    const auto& pointer = Type(array.at("element"));
                    const auto count = array.at("count").get<uint64_t>();
                    if (count > limits.maxArray || pointer.at("size") != 8)
                        throw std::runtime_error("Invalid union pointer array extent");
                    Json extent{ { "count", choice.at("element_count") }, { "evidence", rule.at("evidence") } };
                    result["value"] = Json::array();
                    for (uint64_t i = 0; i < count; i++)
                        result["value"].push_back(Pointer(pointer, Add(at, i * 8), depth + 1, &extent));
                } else if (choice.contains("inline_count")) {
                    const auto& array = Type(memberType);
                    if (array.at("kind") != "array")
                        throw std::runtime_error("Inline extent requires an array member");
                    const auto count = Count(choice.at("inline_count"));
                    const auto element = array.at("element").get<std::string>();
                    const auto stride = Type(element).at("size").get<uint64_t>();
                    if (!stride || count > limits.maxArray ||
                        count > limits.maxNodes - std::min(nodes, limits.maxNodes) ||
                        count > (limits.maxBytes - this->bytes) / stride)
                        throw std::runtime_error("Inline extent exceeds traversal budget");
                    result["count"] = count;
                    result["value"] = Json::array();
                    for (uint64_t i = 0; i < count; i++)
                        result["value"].push_back(Value(element, Add(at, i * stride), depth + 1));
                } else
                    result["value"] = Value(memberType, at, depth + 1);
                if (choice.value("external", false) &&
                    std::any_of(bytes.begin(), bytes.end(), [](uint8_t v) { return v != 0; })) {
                    result["status"] = "external_payload_required";
                    external++;
                }
                return result;
            }
            result["status"] = "unknown_selector";
        } else
            result["status"] = "selector_required";
        unions++;
        return result;
    }
    Json Inspector::Pointer(const Json& type, uint64_t address, size_t depth, const Json* rule) {
        const auto target = type.at("target").get<std::string>();
        const auto ptr = Unsigned(address, 8);
        if (!ptr)
            return nullptr;
        Json result{ { "address", Hex(ptr) }, { "type", target } };
        if (rule && rule->contains("runtime")) {
            result["status"] = "runtime_reference";
            result["reason"] = rule->at("runtime");
            result["reference_evidence"] = rule->at("evidence");
            if (rule->contains("serialized_source"))
                result["serialized_source"] = rule->at("serialized_source");
            ++runtime;
            return result;
        }
        // Assets are named references. Each asset's own entry is the unit of
        // export, so a weapon's image reference must not duplicate an image
        // and its GPU state inside every weapon JSON document.
        for (const auto& pool : database.Profile(profileId).at("pools")) {
            if (pool.at("root_type") != target)
                continue;
            if (rule && Count(rule->at("count")) != 1)
                break;
            result["asset_reference"] = true;
            auto namePointer = Unsigned(Add(ptr, pool.at("name_offset").get<uint64_t>()), 8);
            MemoryReader bounded = [this](uint64_t a, void* b, size_t n) {
                auto data = Read(a, n);
                std::memcpy(b, data.data(), n);
                return true;
            };
            StoreString(result, "name", ReadString(bounded, namePointer, limits.maxString));
            return result;
        }
        if (!rule && (target == "char" || target == "const char")) {
            MemoryReader bounded = [this](uint64_t a, void* b, size_t n) {
                auto data = Read(a, n);
                std::memcpy(b, data.data(), n);
                return true;
            };
            StoreString(result, "string", ReadString(bounded, ptr, limits.maxString));
        } else if (rule) {
            const auto count = Count(rule->at("count"));
            const auto stride = Type(target).at("size").get<uint64_t>();
            if (stride == 1 && Type(target).at("kind") == "scalar") {
                if (count > limits.maxBytes - bytes)
                    throw std::runtime_error("Byte array exceeds traversal budget");
                result["count"] = count;
                result["extent_evidence"] = rule->at("evidence");
                result["bytes"] = HexBytes(Read(ptr, count));
                return result;
            }
            if (!stride || count > limits.maxArray || count > limits.maxNodes - std::min(nodes, limits.maxNodes) ||
                count > (limits.maxBytes - bytes) / stride)
                throw std::runtime_error("Pointer extent exceeds traversal budget");
            result["count"] = count;
            result["extent_evidence"] = rule->at("evidence");
            if (!visited.emplace(ptr, target, count).second) {
                result["status"] = "reference_already_exported";
                return result;
            }
            result["values"] = Json::array();
            for (uint64_t i = 0; i < count; i++)
                result["values"].push_back(Value(
                    target,
                    Add(ptr, i * stride),
                    depth + 1,
                    rule->contains("pointee") ? &rule->at("pointee") : nullptr
                ));
        } else if (target.starts_with("ID3D") || target.find("__fastcall(") != std::string::npos) {
            result["status"] = "runtime_reference";
            ++runtime;
        } else {
            unresolved++;
            result["status"] = "pointer_extent_required";
        }
        return result;
    }
    std::vector<uint8_t> Inspector::Read(uint64_t address, size_t length) {
        if (length > limits.maxBytes - bytes)
            throw std::runtime_error("Byte budget exceeded");
        Add(address, length);
        bytes += length;
        std::vector<uint8_t> data(length);
        if (length && (!address || !read(address, data.data(), length)))
            throw std::runtime_error("Unreadable memory at " + Hex(address));
        return data;
    }
    uint64_t Inspector::Unsigned(uint64_t address, size_t size) {
        if (size > 8)
            throw std::runtime_error("Invalid integer width");
        auto data = Read(address, size);
        uint64_t value{};
        std::memcpy(&value, data.data(), size);
        return value;
    }
    Json Inspector::Value(const std::string& name, uint64_t address, size_t depth, const Json* rule) {
        try {
            if (++nodes > limits.maxNodes || depth > limits.maxDepth)
                throw std::runtime_error("Traversal limit exceeded");
            const auto& type = Type(name);
            const auto kind = type.at("kind").get<std::string>();
            const auto size = type.at("size").get<size_t>();
            if (kind == "pointer") {
                return Pointer(type, address, depth, rule);
            }
            if (kind == "array") {
                const auto count = type.at("count").get<size_t>();
                if (count > limits.maxArray)
                    throw std::runtime_error("Array limit exceeded");
                if (rule && (!rule->contains("elements") || rule->at("elements").size() != count))
                    throw std::runtime_error("Pointer array rule length mismatch");
                auto element = type.at("element").get<std::string>();
                auto stride = Type(element).at("size").get<size_t>();
                if (count && stride > size / count)
                    throw std::runtime_error("Array size mismatch");
                Json result = Json::array();
                for (size_t i = 0; i < count; i++) {
                    const Json* extent = rule ? &rule->at("elements").at(i) : nullptr;
                    if (extent && extent->is_null())
                        extent = nullptr;
                    result.push_back(Value(element, Add(address, i * stride), depth + 1, extent));
                }
                return result;
            }
            if (kind == "struct" || kind == "union") {
                Json result = Json::object();
                contexts.emplace_back(name, address);
                struct PopContext {
                    decltype(contexts)& stack;
                    ~PopContext() { stack.pop_back(); }
                } pop{ contexts };
                if (kind == "union") {
                    if (ContainsPointers(name) || (database.document.contains("union_rules") &&
                                                   database.document.at("union_rules").contains(name))) {
                        return Union(name, type, address, depth);
                    }
                    // Value-only unions have no external data to lose. Export
                    // each explicitly labelled bitwise view, including vectors
                    // and packed flags, without pretending one view is active.
                    result["$union_views"] = true;
                }
                for (const auto& member : type.at("members")) {
                    auto offset = member.at("offset_bits").get<uint64_t>();
                    auto bits = member.at("size_bits").get<size_t>();
                    auto memberType = member.at("type").get<std::string>();
                    auto memberName = member.at("name").get<std::string>();
                    if (offset % 8 || bits % 8 || memberType.find(" : ") != std::string::npos) {
                        if (bits > 64 || bits + offset % 8 > 64)
                            throw std::runtime_error("Bitfield exceeds integer width");
                        auto value = Unsigned(Add(address, offset / 8), (offset % 8 + bits + 7) / 8) >> (offset % 8);
                        result[memberName] = bits == 64 ? value : value & ((uint64_t{ 1 } << bits) - 1);
                    } else {
                        const Json* rule{};
                        if (kind != "union" && database.document.contains("pointer_rules")) {
                            const auto& rules = database.document.at("pointer_rules");
                            if (rules.contains(name) && rules.at(name).contains(memberName))
                                rule = &rules.at(name).at(memberName);
                        }
                        result[memberName] = Value(memberType, Add(address, offset / 8), depth + 1, rule);
                    }
                }
                return result;
            }
            if (size == 0)
                return Json{ { "status", "zero_sized_type" } };
            if (kind == "opaque")
                return Json{ { "bytes", HexBytes(Read(address, size)) },
                             { "status", "opaque_runtime_record" },
                             { "reason", type.at("reason") } };
            if (name == "float" || name == "double") {
                auto data = Read(address, size);
                double value{};
                if (size == 4) {
                    float f{};
                    std::memcpy(&f, data.data(), 4);
                    value = f;
                } else if (size == 8)
                    std::memcpy(&value, data.data(), 8);
                else
                    throw std::runtime_error("Invalid floating point width");
                if (!std::isfinite(value))
                    return Json{ { "bits", HexBytes(data) }, { "status", "nonfinite" } };
                return value;
            }
            if (size > 8)
                return Json{ { "bytes", HexBytes(Read(address, size)) } };
            auto value = Unsigned(address, size);
            if (name == "bool" || name == "_BOOL1")
                return value != 0;
            if (kind == "enum") {
                Json result{ { "value", value } };
                for (auto it = type.at("values").begin(); it != type.at("values").end(); ++it) {
                    if (it.value() == value) {
                        result["name"] = it.key();
                        break;
                    }
                }
                return result;
            }
            bool isSigned = name.find("unsigned") == std::string::npos && name.find("uint") == std::string::npos &&
                            (name == "char" || name == "short" || name == "int" || name == "long" ||
                             name.find("__int") != std::string::npos);
            if (isSigned) {
                if (size < 8 && (value & (uint64_t{ 1 } << (size * 8 - 1))))
                    value |= ~((uint64_t{ 1 } << (size * 8)) - 1);
                return static_cast<int64_t>(value);
            }
            return value;
        } catch (const std::exception& e) {
            errors++;
            return Json{ { "$error", e.what() }, { "type", name }, { "address", Hex(address) } };
        }
    }
    Json Inspector::Inspect(const std::string& type, uint64_t address) {
        bytes = nodes = errors = unresolved = unions = external = runtime = 0;
        visited.clear();
        contexts.clear();
        pointerTypes.clear();
        visited.emplace(address, type, 1);
        auto value = Value(type, address, 0);
        Json issues = Json::array();
        size_t issueCount{};
        std::function<void(const Json&, const std::string&)> collect = [&](const Json& node, const std::string& path) {
            if (node.is_object()) {
                const auto status = node.contains("status") && node.at("status").is_string()
                                        ? node.at("status").get<std::string>()
                                        : std::string{};
                if (node.contains("$error") || status == "pointer_extent_required" || status == "selector_required" ||
                    status == "unknown_selector" || status == "external_payload_required" ||
                    status == "resident_payload_unavailable") {
                    ++issueCount;
                    if (issues.size() < 64)
                        issues.push_back(
                            Json{ { "path", path },
                                  { "reason", node.contains("$error") ? node.at("$error") : Json(status) } }
                        );
                }
                for (const auto& [key, child] : node.items()) {
                    if (child.is_structured())
                        collect(child, path + "/" + key);
                }
            } else if (node.is_array()) {
                for (size_t i = 0; i < node.size(); ++i) {
                    if (node[i].is_structured())
                        collect(node[i], path + "/" + std::to_string(i));
                }
            }
        };
        collect(value, "fields");
        return Json{ { "type", type },
                     { "address", Hex(address) },
                     { "fields", std::move(value) },
                     { "read_errors", errors },
                     { "unresolved_pointers", unresolved },
                     { "unresolved_unions", unions },
                     { "external_payloads", external },
                     { "runtime_references", runtime },
                     { "issues", std::move(issues) },
                     { "issue_count", issueCount },
                     { "bytes_read", bytes },
                     { "nodes", nodes } };
    }
} // namespace tool::mw19::schema
