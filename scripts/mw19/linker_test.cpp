#include "tools/mw19/mw19_linker.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace tool::mw19;
template<typename F>
void Reject(F&& f) {
    try {
        f();
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error("Invalid linker input accepted");
}
int main(int argc, char** argv) {
    try {
        if (argc != 4)
            throw std::runtime_error("schema, fixture and output directory required");
        schema::Database db(std::filesystem::path{ argv[1] });
        std::ifstream file(argv[2]);
        schema::Json assets;
        file >> assets;
        const auto& profile = db.Profile("replay-1.20");
        auto zone = linker::Link(profile, assets);
        if (zone.assets.size() != 8 || zone.blocks[6] == 0 || zone.blocks[5] == 0 || zone.blocks[1] == 0)
            throw std::runtime_error("Missing assets or stream reservations");
        Reject([&] { linker::Link(db.Profile("game-test"), assets); });
        Reject([&] { linker::Link(profile, assets, 16); });
        auto duplicate = assets;
        duplicate.push_back(assets[0]);
        Reject([&] { linker::Link(profile, duplicate); });
        auto invalid = assets;
        invalid[0]["pool"] = "weapon";
        Reject([&] { linker::Link(profile, invalid); });
        invalid = assets;
        invalid[0]["name"] = std::string("bad\0name", 8);
        Reject([&] { linker::Link(profile, invalid); });
        invalid = assets;
        for (auto& asset : invalid)
            if (asset["pool"] == "netconststrings")
                asset["string_type"] = -1;
        Reject([&] { linker::Link(profile, invalid); });
        auto inferred = assets;
        for (auto& asset : inferred)
            if (asset["pool"] == "stringtable")
                asset.erase("hashes");
        if (linker::Link(profile, inferred).body != zone.body)
            throw std::runtime_error("Inferred table hashes differ");
        Reject([&] { linker::Pack(zone, true, 0); });
        Reject([&] { linker::Pack(zone, true, 0x10001); });
        auto empty = linker::Link(profile, schema::Json::array());
        if (empty.body.size() != 32)
            throw std::runtime_error("Invalid empty zone");
        const auto dir = std::filesystem::path(argv[3]);
        auto write = [&](const char* name, const std::vector<uint8_t>& bytes) {
            std::ofstream out(dir / name, std::ios::binary);
            out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            if (!out)
                throw std::runtime_error("Cannot write synthetic test fixture");
        };
        write("expected.body", zone.body);
        write("stored.ff", linker::Pack(zone, false, 31));
        write("lz4.ff", linker::Pack(zone, true, 31));
        schema::Json expected{ { "blocks", zone.blocks }, { "assets", zone.assets } };
        std::ofstream(dir / "layout.json") << expected.dump(2);
        std::cout << "IW8 linker: eight asset writers, profile separation, duplicate/name/budget guards, multi-block "
                     "stored/LZ4 fixtures passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
