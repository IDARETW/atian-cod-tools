#pragma once
#include "mw19_schema.hpp"
#include <stdexcept>

namespace tool::mw19::schema {
    // The callback retrieves exactly one decoded package entry by its content
    // key. Missing entries must throw PayloadUnavailable; corrupt data fails.
    using PackageReader = std::function<std::vector<uint8_t>(uint64_t key, size_t decodedBytes)>;
    class PayloadUnavailable : public std::runtime_error {
      public:
        std::string reason;
        PayloadUnavailable(std::string code, const std::string& message)
            : std::runtime_error(message), reason(std::move(code)) {}
    };
    struct Payload {
        std::string extension;
        std::vector<uint8_t> data;
        std::string format;
    };
    // Throws on a malformed or unsupported asset, never silently writes a
    // partial payload. Conventional formats and structured records retain
    // distinct format identifiers; JSON is never labelled as a native binary.
    bool HasPayloadExporter(const std::string& pool);
    Payload ExportPayload(
        const MemoryReader& read, const std::string& pool, uint64_t address, size_t maxBytes = 128 * 1024 * 1024,
        const std::string& profile = "replay-1.20", const PackageReader& packages = {}
    );
    Payload
    ExportStructuredAsset(const Json& profile, const Json& pool, Json inspection, size_t maxBytes = 128 * 1024 * 1024);
} // namespace tool::mw19::schema
