#ifndef MW19_SCHEMA_STANDALONE
#include <includes.hpp>
#endif
#include "mw19_mesh.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

namespace tool::mw19::schema {
    namespace {
        template<typename T>
        T Get(std::span<const uint8_t> data, size_t offset) {
            if (offset > data.size() || sizeof(T) > data.size() - offset)
                throw std::runtime_error("Truncated mesh data");
            T value;
            std::memcpy(&value, data.data() + offset, sizeof(value));
            return value;
        }
        void Range(std::span<const uint8_t> data, size_t offset, size_t size) {
            if (offset > data.size() || size > data.size() - offset)
                throw std::runtime_error("Mesh attribute extends beyond shared buffer");
        }
        float Half(uint16_t bits) {
            const auto exponent = (bits >> 10) & 31;
            const auto mantissa = bits & 1023;
            if (exponent == 31)
                throw std::runtime_error("Nonfinite mesh texture coordinate");
            const float value = exponent ? std::ldexp(1.0f + mantissa / 1024.0f, exponent - 15)
                                         : std::ldexp(static_cast<float>(mantissa), -24);
            return (bits & 0x8000) ? -value : value;
        }
        std::array<float, 3> Normal(uint32_t packed) {
            // QuatDec3n: three stored components (10/10/9 bits), bit29 is
            // handedness, and bits30..31 select the omitted component.
            // Source scalar decoder and April 2020 Greyhound agree on this
            // representation. This is separate from the native position proof.
            const std::array<float, 3> stored{ (packed & 1023) * 0.001382418f - 0.70710677f,
                                               ((packed >> 10) & 1023) * 0.001382418f - 0.70710677f,
                                               ((packed >> 20) & 511) * 0.0027675412f - 0.70710677f };
            float sum = 0;
            for (float value : stored)
                sum += value * value;
            if (sum > 1.00001f)
                throw std::runtime_error("Invalid packed mesh quaternion");
            std::array<float, 4> q{};
            size_t next = 0;
            for (size_t i = 0; i < q.size(); ++i)
                q[i] = i == (packed >> 30) ? std::sqrt(std::max(0.0f, 1.0f - sum)) : stored[next++];
            const auto [x, y, z, w] = q;
            std::array<float, 3> result{ 2 * (y * w + x * z), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y) };
            float length = std::sqrt(result[0] * result[0] + result[1] * result[1] + result[2] * result[2]);
            if (!std::isfinite(length) || length < 0.5f)
                throw std::runtime_error("Invalid mesh normal");
            for (auto& value : result)
                value /= length;
            return result;
        }
        template<typename T>
        void Append(std::vector<uint8_t>& data, const T& value) {
            const auto bytes = reinterpret_cast<const uint8_t*>(&value);
            data.insert(data.end(), bytes, bytes + sizeof(value));
        }
    } // namespace

    GeometryPayload ExportReplayGeometry(
        const MemoryReader& read, uint64_t address, std::span<const uint8_t> sharedData, size_t maxBytes
    ) {
        size_t remaining = maxBytes;
        auto bytes = [&](uint64_t at, size_t length) {
            if (length > remaining || !at || length > UINT64_MAX - at)
                throw std::runtime_error("Mesh header exceeds read budget or address range");
            remaining -= length;
            std::vector<uint8_t> result(length);
            if (length && !read(at, result.data(), length))
                throw std::runtime_error("Cannot read mesh header");
            return result;
        };
        if (sharedData.size() > remaining)
            throw std::runtime_error("Mesh shared buffer exceeds budget");
        remaining -= sharedData.size();
        const auto root = bytes(address, 96);
        const auto count = Get<uint16_t>(root, 56);
        if (!count)
            throw PayloadUnavailable("geometry_empty", "Model surface set is empty");
        if (count > 4096)
            throw std::runtime_error("Surface count exceeds bounded geometry limit of 4096");
        const auto sharedAddress = Get<uint64_t>(root, 48);
        const auto surfacesAddress = Get<uint64_t>(root, 8);
        if (!sharedAddress || !surfacesAddress)
            throw PayloadUnavailable("geometry_headers_unavailable", "Model surface headers are unavailable");
        const auto sharedHeader = bytes(sharedAddress, 16);
        if (Get<uint32_t>(sharedHeader, 8) != sharedData.size())
            throw std::runtime_error("Mesh shared size changed or does not match supplied bytes");
        // Native 0x1BB743D steps by 192. Vertex and triangle offsets, bounds
        // and 20-byte vertex / six-byte triangle strides are used at 0x1BB7474.
        const auto headers = bytes(surfacesAddress, static_cast<size_t>(count) * 192);
        Json doc{
            { "asset", { { "version", "2.0" }, { "generator", "AtianCodTools MW2019 Replay geometry" } } },
            { "scene", 0 },
            { "scenes", Json::array({ Json{ { "nodes", Json::array({ 0 }) } } }) },
            { "nodes",
              Json::array(
                  { Json{ { "mesh", 0 },
                          { "rotation", Json::array({ -0.7071067811865476, 0.0, 0.0, 0.7071067811865476 }) },
                          { "scale", Json::array({ 0.0254, 0.0254, 0.0254 }) } } }
              ) },
            { "meshes", Json::array({ Json{ { "primitives", Json::array() } } }) },
            { "bufferViews", Json::array() },
            { "accessors", Json::array() },
            { "extras",
              { { "scope", "base surface geometry" },
                { "sourceCoordinates", "right-handed Z-up, inches" },
                { "omitted", Json::array({ "skeleton", "skinning", "materials", "morphs", "subdivision" }) } } }
        };
        GeometryPayload result;
        std::vector<uint8_t> bin;
        auto accessor = [&](size_t offset,
                            size_t length,
                            size_t stride,
                            size_t componentOffset,
                            size_t elements,
                            int componentType,
                            const char* type,
                            bool normalized,
                            int target) {
            const auto view = doc["bufferViews"].size();
            Json v{ { "buffer", 0 }, { "byteOffset", offset }, { "byteLength", length }, { "target", target } };
            if (stride)
                v["byteStride"] = stride;
            doc["bufferViews"].push_back(std::move(v));
            Json a{ { "bufferView", view },
                    { "byteOffset", componentOffset },
                    { "componentType", componentType },
                    { "count", elements },
                    { "type", type } };
            if (normalized)
                a["normalized"] = true;
            const auto index = doc["accessors"].size();
            doc["accessors"].push_back(std::move(a));
            return index;
        };
        for (size_t s = 0; s < count; ++s) {
            const auto h = std::span<const uint8_t>(headers).subspan(s * 192, 192);
            const size_t vertices = Get<uint16_t>(h, 2), triangles = Get<uint16_t>(h, 4);
            if (!vertices || !triangles) {
                if (vertices || triangles)
                    throw std::runtime_error("Mesh surface has inconsistent empty counts");
                continue;
            }
            if (Get<uint64_t>(h, 72) != sharedAddress)
                throw PayloadUnavailable("geometry_shared_mismatch", "Surface uses a different shared buffer");
            const auto vertexOffset = Get<uint32_t>(h, 36), indexOffset = Get<uint32_t>(h, 40);
            const auto colorOffset = Get<uint32_t>(h, 48), uvOffset = Get<uint32_t>(h, 52);
            const bool colors = colorOffset != UINT32_MAX, secondUv = uvOffset != UINT32_MAX;
            Range(sharedData, vertexOffset, vertices * 20);
            Range(sharedData, indexOffset, triangles * 6);
            if (colors)
                Range(sharedData, colorOffset, vertices * 4);
            if (secondUv)
                Range(sharedData, uvOffset, vertices * 4);
            std::array<float, 3> center{}, extent{};
            for (size_t axis = 0; axis < 3; ++axis) {
                center[axis] = Get<float>(h, 144 + axis * 4);
                extent[axis] = Get<float>(h, 156 + axis * 4);
                if (!std::isfinite(center[axis]) || !std::isfinite(extent[axis]) || extent[axis] < 0)
                    throw std::runtime_error("Invalid mesh surface bounds");
            }
            const float scale = *std::max_element(extent.begin(), extent.end());
            const size_t stride = 32 + (secondUv ? 8 : 0) + (colors ? 4 : 0);
            const size_t vertexBytes = vertices * stride, indexBytes = triangles * 6;
            // Reserve the complete expanded surface before appending any data.
            if (vertexBytes + indexBytes + 3 > remaining)
                throw std::runtime_error("Expanded geometry exceeds output budget");
            remaining -= vertexBytes + indexBytes + 3;
            std::array<float, 3> low{}, high{};
            low.fill(std::numeric_limits<float>::infinity());
            high.fill(-std::numeric_limits<float>::infinity());
            const auto start = bin.size();
            for (size_t v = 0; v < vertices; ++v) {
                const size_t at = vertexOffset + v * 20;
                const auto packed = Get<uint64_t>(sharedData, at);
                for (size_t axis = 0; axis < 3; ++axis) {
                    // Native 0x1992D20: a single MAX half-size for all axes.
                    const float value =
                        (static_cast<float>((packed >> (21 * axis)) & 0x1fffff) * 4.768373855768004e-7f * 2.0f - 1.0f) *
                            scale +
                        center[axis];
                    if (!std::isfinite(value))
                        throw std::runtime_error("Nonfinite decoded mesh position");
                    low[axis] = std::min(low[axis], value);
                    high[axis] = std::max(high[axis], value);
                    Append(bin, value);
                }
                for (float value : Normal(Get<uint32_t>(sharedData, at + 16)))
                    Append(bin, value);
                Append(bin, Half(Get<uint16_t>(sharedData, at + 12)));
                Append(bin, Half(Get<uint16_t>(sharedData, at + 14)));
                if (secondUv) {
                    Append(bin, Half(Get<uint16_t>(sharedData, uvOffset + v * 4)));
                    Append(bin, Half(Get<uint16_t>(sharedData, uvOffset + v * 4 + 2)));
                }
                if (colors)
                    Append(bin, Get<uint32_t>(sharedData, colorOffset + v * 4));
            }
            Json attributes;
            const auto position = accessor(start, vertexBytes, stride, 0, vertices, 5126, "VEC3", false, 34962);
            doc["accessors"][position]["min"] = low;
            doc["accessors"][position]["max"] = high;
            attributes["POSITION"] = position;
            attributes["NORMAL"] = accessor(start, vertexBytes, stride, 12, vertices, 5126, "VEC3", false, 34962);
            attributes["TEXCOORD_0"] = accessor(start, vertexBytes, stride, 24, vertices, 5126, "VEC2", false, 34962);
            if (secondUv)
                attributes["TEXCOORD_1"] =
                    accessor(start, vertexBytes, stride, 32, vertices, 5126, "VEC2", false, 34962);
            if (colors)
                attributes["COLOR_0"] =
                    accessor(start, vertexBytes, stride, stride - 4, vertices, 5121, "VEC4", true, 34962);
            const auto indexStart = bin.size();
            for (size_t i = 0; i < triangles * 3; ++i) {
                const auto index = Get<uint16_t>(sharedData, indexOffset + i * 2);
                if (index >= vertices)
                    throw std::runtime_error("Triangle index is outside its surface vertex range");
                Append(bin, index);
            }
            const auto indices = accessor(indexStart, indexBytes, 0, 0, triangles * 3, 5123, "SCALAR", false, 34963);
            while (bin.size() % 4)
                bin.push_back(0);
            doc["meshes"][0]["primitives"].push_back(
                Json{ { "attributes", std::move(attributes) },
                      { "indices", indices },
                      { "mode", 4 },
                      { "extras", { { "sourceSurface", s }, { "sourceFlags", Get<uint16_t>(h, 0) } } } }
            );
            ++result.surfaces;
            result.vertices += vertices;
            result.triangles += triangles;
        }
        if (!result.surfaces)
            throw PayloadUnavailable("geometry_empty", "No nonempty triangle surfaces");
        doc["buffers"] = Json::array({ Json{ { "byteLength", bin.size() } } });
        auto json = doc.dump();
        while (json.size() % 4)
            json += ' ';
        if (json.size() > remaining || remaining - json.size() < 28 || json.size() + bin.size() + 28 > UINT32_MAX)
            throw std::runtime_error("GLB exceeds output budget");
        result.payload = { ".geometry.glb", {}, "glTF 2.0 base surface geometry" };
        auto& glb = result.payload.data;
        glb.reserve(json.size() + bin.size() + 28);
        Append(glb, uint32_t{ 0x46546c67 });
        Append(glb, uint32_t{ 2 });
        Append(glb, static_cast<uint32_t>(json.size() + bin.size() + 28));
        Append(glb, static_cast<uint32_t>(json.size()));
        Append(glb, uint32_t{ 0x4e4f534a });
        glb.insert(glb.end(), json.begin(), json.end());
        Append(glb, static_cast<uint32_t>(bin.size()));
        Append(glb, uint32_t{ 0x004e4942 });
        glb.insert(glb.end(), bin.begin(), bin.end());
        return result;
    }
} // namespace tool::mw19::schema
