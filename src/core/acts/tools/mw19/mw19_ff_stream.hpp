#pragma once
#include <array>
#include <span>
#include <vector>
#include <cstring>
#include <stdexcept>
#include <cstdint>
#include <algorithm>
#include <string>
#include <functional>

namespace tool::mw19::replay {
    struct StreamTraceEvent {
        const char* operation;
        size_t blockBefore, offsetBefore, inputBefore;
        size_t blockAfter, offsetAfter, inputAfter;
        uint64_t value;
        size_t size;
    };

    // Replay has eight native streams. The file header's eleven reservation
    // values must not be confused with the native stream count.
    class Streams {
        struct Frame {
            size_t block, start;
        };
        std::array<std::span<uint8_t>, 8> memory;
        std::array<size_t, 8> offsets{};
        std::vector<Frame> stack;
        std::vector<std::array<size_t, 8>> assets;
        std::span<const uint8_t> input;
        uint8_t*& cursor;
        size_t block{}, consumed{}, shared{};
        std::function<void(const StreamTraceEvent&)> trace;

        void Emit(const char* operation, size_t oldBlock, size_t oldOffset, size_t oldInput,
                  uint64_t value = 0, size_t size = 0) {
            if (trace)
                trace({operation, oldBlock, oldOffset, oldInput,
                       block, offsets[block], consumed, value, size});
        }

        void Sync() {
            auto begin = reinterpret_cast<uintptr_t>(memory[block].data());
            auto ptr = reinterpret_cast<uintptr_t>(cursor);
            if (ptr < begin || ptr - begin > memory[block].size())
                throw std::runtime_error("Replay native stream cursor escaped its reservation");
            offsets[block] = ptr - begin;
        }
        void Publish() { cursor = memory[block].data() + offsets[block]; }

      public:
        Streams(std::array<std::span<uint8_t>, 8> memory, std::span<const uint8_t> input, uint8_t*& cursor)
            : memory(memory), input(input), cursor(cursor) {
            Publish();
        }
        size_t Consumed() const { return consumed; }
        size_t InputSize() const { return input.size(); }
        size_t Remaining() const { return input.size() - consumed; }
        size_t Block() const { return block; }
        size_t Depth() const { return stack.size(); }
        void SetTrace(std::function<void(const StreamTraceEvent&)> sink) { trace = std::move(sink); }
        std::span<const uint8_t> Take(size_t size) {
            if (size > Remaining())
                throw std::runtime_error("Truncated Replay file header");
            auto result = input.subspan(consumed, size);
            consumed += size;
            return result;
        }
        void Check(size_t size) {
            Sync();
            if (size > memory[block].size() - offsets[block])
                throw std::runtime_error("Replay stream reservation exhausted (block " + std::to_string(block) +
                    ", offset " + std::to_string(offsets[block]) + ", requested " + std::to_string(size) +
                    ", reserved " + std::to_string(memory[block].size()) + ")");
        }
        void Advance(size_t size) {
            Check(size);
            offsets[block] += size;
            Publish();
        }
        void Align(size_t mask) {
            if (mask > 0xfffff || (mask & (mask + 1)))
                throw std::runtime_error("Invalid Replay stream alignment");
            Sync();
            auto oldBlock = block, oldOffset = offsets[block], oldInput = consumed;
            auto padding = (0 - reinterpret_cast<uintptr_t>(cursor)) & mask;
            Advance(padding);
            Emit("align", oldBlock, oldOffset, oldInput, mask, padding);
        }
        void Push(size_t next) {
            if (next >= memory.size() || stack.size() >= 64)
                throw std::runtime_error("Invalid Replay stream push");
            Sync();
            auto oldBlock = block, oldOffset = offsets[block], oldInput = consumed;
            stack.push_back({ block, offsets[next] });
            block = next;
            Publish();
            Emit("push", oldBlock, oldOffset, oldInput, next);
        }
        void Pop() {
            if (stack.empty())
                throw std::runtime_error("Replay stream stack underflow");
            Sync();
            auto oldBlock = block, oldOffset = offsets[block], oldInput = consumed;
            auto previous = stack.back();
            if (block == 1)
                offsets[block] = previous.start;
            else if (block == 2)
                Align(31);
            stack.pop_back();
            block = previous.block;
            Publish();
            Emit("pop", oldBlock, oldOffset, oldInput);
        }
        bool Contains(uint64_t address, size_t size) const {
            for (auto m : memory) {
                auto start = reinterpret_cast<uintptr_t>(m.data());
                if (address >= start && address - start <= m.size() && size <= m.size() - (address - start))
                    return true;
            }
            return false;
        }
        void Load(bool atStart, void* destination, size_t size) {
            if (!atStart || !size)
                return;
            Check(size);
            auto oldBlock = block, oldOffset = offsets[block], oldInput = consumed;
            if (destination != cursor)
                throw std::runtime_error("Replay Load_Stream destination differs from cursor");
            if (block == 4)
                std::memset(destination, 0, size);
            else {
                if (size > Remaining())
                    throw std::runtime_error("Truncated Replay serialized stream");
                std::memcpy(destination, input.data() + consumed, size);
                consumed += size;
            }
            Advance(size);
            Emit("load", oldBlock, oldOffset, oldInput, atStart, size);
        }
        void String(char** value) {
            if (block == 4)
                throw std::runtime_error("Replay string in runtime stream");
            auto available = input.subspan(consumed);
            auto end = static_cast<const uint8_t*>(
                std::memchr(available.data(), 0, std::min<size_t>(available.size(), 65536))
            );
            if (!end)
                throw std::runtime_error("Unterminated Replay string");
            Sync();
            auto oldBlock = block, oldOffset = offsets[block], oldInput = consumed;
            Emit("string", oldBlock, oldOffset, oldInput, 0, size_t(end - available.data()) + 1);
            Load(true, *value, size_t(end - available.data()) + 1);
        }
        void BeginAsset() {
            if (assets.size() >= 64)
                throw std::runtime_error("Replay asset nesting limit exceeded");
            Sync();
            auto oldBlock = block, oldOffset = offsets[block], oldInput = consumed;
            assets.push_back(offsets);
            Emit("begin_asset", oldBlock, oldOffset, oldInput, assets.size());
        }
        void EndAsset() {
            if (assets.empty())
                throw std::runtime_error("Replay asset stack underflow");
            Sync();
            auto oldBlock = block, oldOffset = offsets[block], oldInput = consumed;
            assets.pop_back();
            Emit("end_asset", oldBlock, oldOffset, oldInput, assets.size());
        }
        void SharedPush() {
            if (++shared > 256)
                throw std::runtime_error("Replay shared data nesting limit exceeded");
        }
        void SharedPop() {
            if (!shared)
                throw std::runtime_error("Replay shared data stack underflow");
            --shared;
        }
        uint8_t* Resolve(uint64_t packed, bool alias) {
            Sync();
            auto oldBlock = block, oldOffset = offsets[block], oldInput = consumed;
            size_t index = (packed >> 32) & 15;
            uint64_t offset = uint32_t(packed - 1);
            if (packed & ~UINT64_C(0x1fffffffff))
                throw std::runtime_error("Invalid Replay packed pointer bits");
            if (index >= memory.size())
                throw std::runtime_error("Invalid Replay packed stream index");
            if (packed & UINT64_C(0x1000000000)) {
                if (assets.empty())
                    throw std::runtime_error("Replay relative offset outside an asset");
                offset += assets.front()[index];
            }
            if (offset >= memory[index].size() || (alias && memory[index].size() - offset < 8))
                throw std::runtime_error("Replay packed pointer outside stream reservation: block " +
                    std::to_string(index) + ", offset " + std::to_string(offset) +
                    ", reserved " + std::to_string(memory[index].size()));
            uint8_t* value = memory[index].data() + offset;
            if (alias)
                std::memcpy(&value, value, 8);
            Emit(alias ? "alias" : "resolve", oldBlock, oldOffset, oldInput, packed, offset);
            return value;
        }
        void** Insert() {
            Push(5);
            Align(7);
            Check(8);
            auto result = reinterpret_cast<void**>(cursor);
            *result = nullptr;
            Advance(8);
            Pop();
            return result;
        }
        const void* Temporary(size_t size, size_t alignment) {
            if (!alignment)
                throw std::runtime_error("Zero temporary alignment");
            Sync();
            auto oldBlock = block, oldOffset = offsets[block], oldInput = consumed;
            Emit("temporary", oldBlock, oldOffset, oldInput, alignment, size);
            Push(1);
            Align(alignment - 1);
            auto result = cursor;
            Load(true, result, size);
            Pop();
            return result;
        }
        void Finish() {
            Sync();
            if (!stack.empty() || !assets.empty() || shared)
                throw std::runtime_error("Unbalanced Replay loader stacks");
            if (Remaining())
                throw std::runtime_error("Replay loader left unread serialized bytes: " + std::to_string(Remaining()));
        }
    };
} // namespace tool::mw19::replay
