#include "tools/mw19/mw19_mesh.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <io.h>
#include <fcntl.h>

using namespace tool::mw19::schema;
int main(int argc, char**) {
    try {
        constexpr uint64_t base = 0x10000;
        std::vector<uint8_t> memory(1024), shared(96);
        auto put = [](auto& b, size_t at, auto value) { std::memcpy(b.data() + at, &value, sizeof(value)); };
        auto check = [](bool ok, const char* message) {
            if (!ok)
                throw std::runtime_error(message);
        };
        MemoryReader read = [&](uint64_t address, void* out, size_t n) {
            if (address < base || address - base > memory.size() || n > memory.size() - (address - base))
                return false;
            std::memcpy(out, memory.data() + address - base, n);
            return true;
        };
        const uint32_t identity = 512 | (512 << 10) | (256 << 20) | (3u << 30);
        auto setup = [&] {
            std::fill(memory.begin(), memory.end(), 0);
            shared.assign(66, 0);
            put(memory, 8, base + 256);
            put(memory, 48, base + 128);
            put(memory, 56, uint16_t{ 1 });
            put(memory, 136, uint32_t{ 66 });
            put(memory, 258, uint16_t{ 3 });
            put(memory, 260, uint16_t{ 1 });
            put(memory, 296, uint32_t{ 60 });
            put(memory, 304, UINT32_MAX);
            put(memory, 308, UINT32_MAX);
            put(memory, 328, base + 128);
            for (size_t i = 0; i < 3; ++i) {
                put(memory, 400 + i * 4, 10.0f * static_cast<float>(i + 1));
                put(memory, 412 + i * 4, static_cast<float>(1 << i));
                put(shared, i * 20 + 16, identity);
                put(shared, 60 + i * 2, static_cast<uint16_t>(i));
            }
            put(shared, 20, uint64_t{ 0x1fffff });
            put(shared, 40, uint64_t{ 0x1fffff } << 21);
            put(shared, 32, uint16_t{ 0x3c00 });
            put(shared, 54, uint16_t{ 0x3c00 });
        };
        auto get32 = [&](const std::vector<uint8_t>& b, size_t at) {
            uint32_t n;
            std::memcpy(&n, b.data() + at, 4);
            return n;
        };
        auto document = [&](const GeometryPayload& g) {
            const auto& b = g.payload.data;
            check(get32(b, 0) == 0x46546c67 && get32(b, 4) == 2 && get32(b, 8) == b.size(), "Invalid GLB envelope");
            const auto n = get32(b, 12);
            check(n % 4 == 0 && get32(b, 16) == 0x4e4f534a, "Invalid GLB JSON chunk");
            check(get32(b, 24 + n) == 0x004e4942 && 28 + n + get32(b, 20 + n) == b.size(), "Invalid GLB binary chunk");
            return Json::parse(b.begin() + 20, b.begin() + 20 + n);
        };
        auto value = [&](const GeometryPayload& g,
                         const Json& doc,
                         const std::string& semantic,
                         size_t vertex,
                         size_t component) {
            const auto index = doc.at("meshes")[0].at("primitives")[0].at("attributes").at(semantic).get<size_t>();
            const auto& a = doc.at("accessors")[index];
            const auto& v = doc.at("bufferViews")[a.at("bufferView").get<size_t>()];
            const size_t at = 28 + get32(g.payload.data, 12) + v.at("byteOffset").get<size_t>() +
                              a.at("byteOffset").get<size_t>() + vertex * v.at("byteStride").get<size_t>() +
                              component * 4;
            float f;
            std::memcpy(&f, g.payload.data.data() + at, 4);
            return f;
        };
        auto fails = [&](auto action, const char* message) {
            bool failed = false;
            try {
                action();
            } catch (const std::exception&) {
                failed = true;
            }
            check(failed, message);
        };
        setup();
        const auto baseline = ExportReplayGeometry(read, base, shared);
        if (argc > 1) {
            _setmode(_fileno(stdout), _O_BINARY);
            std::cout.write(reinterpret_cast<const char*>(baseline.payload.data.data()), baseline.payload.data.size());
            return 0;
        }
        auto d = document(baseline);
        check(baseline.surfaces == 1 && baseline.vertices == 3 && baseline.triangles == 1, "Incorrect geometry counts");
        const float expected[3][3] = { { 6, 16, 26 }, { 14, 16, 26 }, { 6, 24, 26 } };
        for (size_t v = 0; v < 3; ++v)
            for (size_t c = 0; c < 3; ++c)
                check(
                    std::abs(value(baseline, d, "POSITION", v, c) - expected[v][c]) < 1e-5,
                    "Packed position or shared scale incorrect"
                );
        check(
            value(baseline, d, "TEXCOORD_0", 1, 0) == 1 && value(baseline, d, "TEXCOORD_0", 2, 1) == 1,
            "Half UV decoder failed"
        );
        check(value(baseline, d, "NORMAL", 0, 2) > 0.999, "Identity quaternion normal incorrect");
        check(
            d.at("accessors")[0].at("min") == Json::array({ 6.0, 16.0, 26.0 }) &&
                d.at("accessors")[0].at("max") == Json::array({ 14.0, 24.0, 26.0 }),
            "GLB position bounds incorrect"
        );
        // Largest quaternion component selects x,y,z,w; handedness cannot change its normal.
        for (uint32_t largest = 0; largest < 4; ++largest)
            for (uint32_t sign = 0; sign < 2; ++sign) {
                put(shared, 16, (identity & 0x1fffffff) | (largest << 30) | (sign << 29));
                auto g = ExportReplayGeometry(read, base, shared);
                auto j = document(g);
                check(
                    std::abs(value(g, j, "NORMAL", 0, 2) - (largest < 2 ? -1.0f : 1.0f)) < 0.0001f,
                    "Quaternion selector or sign decoded incorrectly"
                );
            }
        setup();
        put(shared, 12, uint16_t{ 1 });
        put(shared, 14, uint16_t{ 0xbc00 });
        auto half = ExportReplayGeometry(read, base, shared);
        auto hd = document(half);
        check(
            value(half, hd, "TEXCOORD_0", 0, 0) == std::ldexp(1.0f, -24) && value(half, hd, "TEXCOORD_0", 0, 1) == -1,
            "Subnormal or signed half failed"
        );
        setup();
        shared.resize(90);
        put(memory, 136, uint32_t{ 90 });
        put(memory, 304, uint32_t{ 66 });
        put(memory, 308, uint32_t{ 78 });
        for (size_t v = 0; v < 3; ++v) {
            put(shared, 66 + v * 4, uint32_t{ 0xff804020 });
            put(shared, 78 + v * 4, uint32_t{ 0x38003c00 });
        }
        auto colored = ExportReplayGeometry(read, base, shared);
        auto cd = document(colored);
        check(
            value(colored, cd, "TEXCOORD_1", 0, 0) == 1 && value(colored, cd, "TEXCOORD_1", 0, 1) == 0.5f,
            "Second UV array decoding failed"
        );
        const auto color = cd.at("meshes")[0].at("primitives")[0].at("attributes").at("COLOR_0").get<size_t>();
        check(
            cd.at("accessors")[color].at("normalized") == true && cd.at("accessors")[color].at("componentType") == 5121,
            "Vertex color representation incorrect"
        );
        setup();
        std::copy(memory.begin() + 256, memory.begin() + 448, memory.begin() + 448);
        put(memory, 56, uint16_t{ 2 });
        auto multi = ExportReplayGeometry(read, base, shared);
        auto md = document(multi);
        check(
            multi.surfaces == 2 && multi.vertices == 6 && md.at("meshes")[0].at("primitives").size() == 2,
            "Multiple surface traversal failed"
        );
        for (const auto& view : md.at("bufferViews"))
            check(view.at("byteOffset").get<size_t>() % 4 == 0, "GLB view alignment incorrect");
        setup();
        put(shared, 64, uint16_t{ 3 });
        fails([&] { ExportReplayGeometry(read, base, shared); }, "Out-of-range triangle index accepted");
        setup();
        put(memory, 292, UINT32_MAX);
        fails([&] { ExportReplayGeometry(read, base, shared); }, "Invalid vertex offset accepted");
        setup();
        put(memory, 296, uint32_t{ 65 });
        fails([&] { ExportReplayGeometry(read, base, shared); }, "Truncated triangle accepted");
        setup();
        put(shared, 12, uint16_t{ 0x7c00 });
        fails([&] { ExportReplayGeometry(read, base, shared); }, "Infinite UV accepted");
        setup();
        put(memory, 412, -1.0f);
        fails([&] { ExportReplayGeometry(read, base, shared); }, "Negative extent accepted");
        setup();
        put(shared, 16, uint32_t{});
        fails([&] { ExportReplayGeometry(read, base, shared); }, "Impossible quaternion accepted");
        setup();
        put(memory, 136, uint32_t{ 65 });
        fails([&] { ExportReplayGeometry(read, base, shared); }, "Changed shared size accepted");
        setup();
        fails([&] { ExportReplayGeometry(read, base, shared, 256); }, "Header read budget ignored");
        fails([&] { ExportReplayGeometry(read, base, shared, 600); }, "Expanded output budget ignored");
        fails([&] { ExportReplayGeometry(read, UINT64_MAX - 90, shared); }, "Address overflow accepted");
        put(memory, 56, uint16_t{ 4097 });
        fails([&] { ExportReplayGeometry(read, base, shared); }, "Surface count limit ignored");
        for (int which = 0; which < 3; ++which) {
            setup();
            if (which == 0)
                put(memory, 8, uint64_t{});
            if (which == 1)
                put(memory, 328, base + 900);
            if (which == 2)
                put(memory, 56, uint16_t{});
            bool unavailable = false;
            try {
                ExportReplayGeometry(read, base, shared);
            } catch (const PayloadUnavailable&) {
                unavailable = true;
            }
            check(unavailable, "Unavailable geometry misreported as valid or corrupt");
        }
        std::cout << "Replay geometry tests passed: packed positions, quaternion normals, UVs/colors, GLB layout, "
                     "topology and bounds. No asset files written.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
