#ifndef MW19_SCHEMA_STANDALONE
#include <includes.hpp>
#endif
#include "mw19_image.hpp"
#include "mw19_image_formats.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>

namespace tool::mw19::schema {
    Payload
    ExportReplayImage(const MemoryReader& read, uint64_t address, size_t maxBytes, const PackageReader& packages) {
        constexpr size_t ddsBytes = 148;
        std::array<uint8_t, 232> image{};
        if (!address || address > UINT64_MAX - image.size() || maxBytes < image.size() ||
            !read(address, image.data(), image.size()))
            throw std::runtime_error("Cannot read Replay image header within budget");
        auto number = [&]<typename T>(size_t offset) {
            T value{};
            std::memcpy(&value, image.data() + offset, sizeof(value));
            return value;
        };
        const auto format = number.operator()<uint32_t>(20);
        const auto flags = number.operator()<uint32_t>(24);
        const auto totalSize = number.operator()<uint32_t>(28);
        const auto width = number.operator()<uint16_t>(36), height = number.operator()<uint16_t>(38);
        const auto depth = number.operator()<uint16_t>(40), elements = number.operator()<uint16_t>(42);
        const auto mipCount = number.operator()<uint8_t>(48);
        const auto pixels = number.operator()<uint64_t>(224);
        const bool streamed = flags & 0x40;
        if (streamed && !packages)
            throw PayloadUnavailable("streamed_image", "Image pixels require streamed package data");
        if (!streamed && !pixels)
            throw PayloadUnavailable("image_uploaded", "CPU image pixels were released after GPU upload");
        if (!format || format >= image_layout::Dxgi.size() || !image_layout::Dxgi[format])
            throw std::runtime_error("Unsupported Replay image pixel format");
        if (!width || !height || !depth || !mipCount)
            throw std::runtime_error("Invalid image dimensions or mip count");
        const auto map = flags & 0x38000;
        const bool cube = map == 0x8000 || map == 0x28000;
        const bool volume = map == 0x10000, oneD = map == 0x18000;
        const bool array = map == 0x20000 || map == 0x28000;
        if (map > 0x28000 || (cube && width != height) || (oneD && height != 1) || (!volume && depth != 1) ||
            !elements || (!array && elements != 1))
            throw std::runtime_error("Invalid image map dimensions or array size");
        const uint32_t arraySize = array ? elements : 1;
        const uint32_t faces = arraySize * (cube ? 6 : 1);
        uint32_t maxMips = 1;
        for (uint32_t size = std::max({ width, height, depth }); size > 1; size >>= 1)
            ++maxMips;
        if (mipCount > maxMips || ((flags & 2) && mipCount != 1))
            throw std::runtime_error("Image mip count exceeds dimensions or no-mipmap flag");
        const bool compressed = format >= 33 && format <= 45;
        const uint32_t unitBytes =
            compressed ? (format == 33 || format == 34 || format == 39 ? 8 : 16) : image_layout::PixelBytes[format];
        if (!unitBytes)
            throw std::runtime_error("Image format has no native byte size");
        struct Mip {
            uint64_t offset, size, stride, pitch;
        };
        std::vector<Mip> levels;
        uint64_t stored = 0, output = ddsBytes;
        for (uint32_t mip = 0; mip < mipCount; ++mip) {
            const uint64_t w = std::max(1u, uint32_t(width) >> mip), h = std::max(1u, uint32_t(height) >> mip);
            const uint64_t d = std::max(1u, uint32_t(depth) >> mip);
            const uint64_t pitch = (compressed ? (w + 3) / 4 : w) * unitBytes;
            const uint64_t size = pitch * (compressed ? (h + 3) / 4 : h) * d;
            const uint64_t stride = (size + 15) & ~uint64_t{ 15 };
            if (size > maxBytes || output > maxBytes || size > (maxBytes - output) / faces)
                throw std::runtime_error("DDS output exceeds byte budget");
            // Replay stores every mip's array slices together, each aligned
            // to 16 bytes (native 0x193BAE0/0x193BA50). DDS stores each slice's
            // complete mip chain together, without this padding.
            if (stored > totalSize || size > totalSize - stored || faces - 1 > (totalSize - stored - size) / stride)
                throw std::runtime_error("Image pixel allocation is shorter than its subresources");
            const uint64_t required = stored + (faces - 1) * stride + size;
            if (!streamed && pixels > UINT64_MAX - required)
                throw std::runtime_error("Image pixel address overflow");
            levels.push_back({ stored, size, stride, pitch });
            stored += stride * faces;
            output += size * faces;
        }
        if (output - ddsBytes > maxBytes - image.size())
            throw std::runtime_error("Image reads exceed byte budget");
        std::vector<uint8_t> streamedPixels;
        if (streamed) {
            // Replay reads the stream count at +0x32, unlike game-test's
            // +0x31 (native 0x139F7B6). Each part contains a growing suffix
            // of the mip chain. 0x13A3280/0x13B32F0 use cumulative-size
            // differences and place it at totalSize - cumulativeSize.
            const auto partCount = number.operator()<uint8_t>(50);
            if (!partCount || partCount > 4 || stored != totalSize || totalSize > maxBytes - image.size())
                throw std::runtime_error("Invalid streamed image allocation or part count");
            struct Part {
                uint64_t key;
                uint32_t offset, size;
            };
            std::vector<Part> parts;
            uint32_t previousSize = 0, previousMips = 0;
            for (uint32_t part = 0; part < partCount; ++part) {
                const size_t at = 56 + part * 40;
                const auto packed = number.operator()<uint32_t>(at + 32);
                const auto cumulativeSize = packed >> 4, cumulativeMips = packed & 15;
                if (cumulativeMips <= previousMips || cumulativeMips > mipCount || cumulativeSize <= previousSize ||
                    cumulativeSize > totalSize)
                    throw std::runtime_error("Invalid streamed image cumulative sizes or mip counts");
                const auto firstMip = mipCount - cumulativeMips;
                if (totalSize - cumulativeSize != levels[firstMip].offset ||
                    number.operator()<uint16_t>(at + 36) != std::max(1u, uint32_t(width) >> firstMip) ||
                    number.operator()<uint16_t>(at + 38) != std::max(1u, uint32_t(height) >> firstMip))
                    throw std::runtime_error("Streamed image part does not match its mip dimensions or offset");
                const auto key = number.operator()<uint64_t>(at);
                if (!key)
                    throw PayloadUnavailable("image_part_unavailable", "Streamed image part has no XPak key");
                parts.push_back({ key, totalSize - cumulativeSize, cumulativeSize - previousSize });
                previousSize = cumulativeSize;
                previousMips = cumulativeMips;
            }
            if (previousSize != totalSize || previousMips != mipCount)
                throw std::runtime_error("Streamed image parts do not cover the complete mip chain");
            streamedPixels.resize(totalSize);
            for (const auto& part : parts) {
                auto bytes = packages(part.key, part.size);
                if (bytes.size() != part.size)
                    throw std::runtime_error("Streamed image package returned an incorrect byte count");
                std::memcpy(streamedPixels.data() + part.offset, bytes.data(), bytes.size());
            }
        }
        std::array<uint32_t, 37> header{};
        header[0] = 0x20534444; // DDS magic
        header[1] = 124;
        header[2] = 0x1007 | (compressed ? 0x80000 : 8) | (mipCount > 1 ? 0x20000 : 0) | (volume ? 0x800000 : 0);
        header[3] = height;
        header[4] = width;
        header[5] = static_cast<uint32_t>(compressed ? levels[0].size : levels[0].pitch);
        header[6] = volume ? depth : 0;
        header[7] = mipCount;
        header[19] = 32;
        header[20] = 4;
        header[21] = 0x30315844; // DDPF_FOURCC, DX10
        header[27] = 0x1000 | (mipCount > 1 ? 0x400008 : 0) | (cube || volume || faces > 1 ? 8 : 0);
        header[28] = (cube ? 0xfe00 : 0) | (volume ? 0x200000 : 0);
        header[32] = image_layout::Dxgi[format];
        header[33] = volume ? 4 : oneD ? 2 : 3;
        header[34] = cube ? 4 : 0;
        header[35] = arraySize;
        header[36] = flags & 0x80000 ? 2 : 0; // Premultiplied alpha, otherwise unspecified.
        Payload result{ ".dds", std::vector<uint8_t>(static_cast<size_t>(output)), "DDS DX10" };
        std::memcpy(result.data.data(), header.data(), ddsBytes);
        size_t destination = ddsBytes;
        for (uint32_t face = 0; face < faces; ++face) {
            for (const auto& mip : levels) {
                if (streamed) {
                    std::memcpy(
                        result.data.data() + destination,
                        streamedPixels.data() + mip.offset + face * mip.stride,
                        static_cast<size_t>(mip.size)
                    );
                } else if (!read(
                               pixels + mip.offset + face * mip.stride,
                               result.data.data() + destination,
                               static_cast<size_t>(mip.size)
                           ))
                    throw std::runtime_error("Cannot read resident image subresource");
                destination += static_cast<size_t>(mip.size);
            }
        }
        return result;
    }
} // namespace tool::mw19::schema
