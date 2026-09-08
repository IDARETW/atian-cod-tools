#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <nlohmann/json.hpp>
#include <set>
#include <tuple>
#include <string>
#include <vector>

namespace tool::mw19::schema {
    using Json = nlohmann::ordered_json;
    using MemoryReader = std::function<bool(uint64_t, void*, size_t)>;

    struct Limits {
        size_t maxBytes{ 128 * 1024 * 1024 };
        size_t maxNodes{ 250000 };
        size_t maxArray{ 65536 };
        size_t maxString{ 65536 };
        size_t maxDepth{ 32 };
    };

    class Database {
      public:
        Json document;
        explicit Database(const std::filesystem::path& path);
        explicit Database(Json value);
        const Json& Profile(const std::string& id) const;
        const Json& Type(const std::string& name) const;
        const Json& Pool(const Json& profile, const std::string& name) const;
        void Validate() const;
    };

    struct AssetEntry {
        uint64_t header;
        uint32_t nextHash;
        uint32_t nextStashed;
        uint8_t inUse;
        uint8_t type;
        uint16_t zoneFlags;
        uint32_t index;
    };

    // The native entry has a 20-byte stride; don't use sizeof(AssetEntry).
    std::vector<AssetEntry> ReadReplayEntries(const MemoryReader& read, uint64_t table, const Json& profile);
    void ValidateProfile(const MemoryReader& read, uint64_t base, const Json& profile);
    std::string ReadString(const MemoryReader& read, uint64_t address, size_t limit = 65536);
    std::filesystem::path AssetPath(const std::filesystem::path& root, std::string name);
    void WriteJsonAtomic(const std::filesystem::path& path, const Json& value);

    class Inspector {
        const Database& database;
        MemoryReader read;
        Limits limits;
        size_t bytes{};
        size_t nodes{};
        size_t errors{};
        size_t unresolved{};
        size_t unions{};
        size_t external{};
        size_t runtime{};
        std::string profileId;
        std::vector<std::pair<std::string, uint64_t>> contexts;
        std::map<std::string, bool> pointerTypes;
        std::set<std::tuple<uint64_t, std::string, uint64_t>> visited;
        const Json& Type(const std::string& name) const;
        bool ContainsPointers(const std::string& name);
        Json Union(const std::string& name, const Json& type, uint64_t address, size_t depth);
        uint64_t Count(const Json& expression, size_t depth = 0);
        Json Pointer(const Json& type, uint64_t address, size_t depth, const Json* rule);
        std::vector<uint8_t> Read(uint64_t address, size_t length);
        Json Value(const std::string& type, uint64_t address, size_t depth, const Json* rule = nullptr);
        uint64_t Unsigned(uint64_t address, size_t size);

      public:
        Inspector(const Database& database, MemoryReader read, Limits limits = {}, std::string profileId = "game-test");
        Json Inspect(const std::string& type, uint64_t address);
    };
} // namespace tool::mw19::schema
