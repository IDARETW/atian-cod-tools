#include <tools/mw19/mw19_ff_stream.hpp>
#include <cassert>
#include <iostream>
#include <memory>
using tool::mw19::replay::Streams;
template<class F>
void Reject(F f) {
    bool rejected = false;
    try {
        f();
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    if (!rejected)
        throw std::runtime_error("Malformed stream was accepted");
}
int main() {
    alignas(256) uint8_t storage[8][256]{};
    std::array<std::span<uint8_t>, 8> blocks;
    for (size_t i = 0; i < 8; ++i)
        blocks[i] = storage[i];
    const uint8_t data[]{ 1, 2, 3, 4, 'a', 'b', 0, 9, 8 };
    uint8_t* cursor{};
    Streams s(blocks, data, cursor);
    s.Load(true, cursor, 4);
    assert(cursor == storage[0] + 4 && s.Consumed() == 4);
    s.BeginAsset();
    s.Push(5);
    s.Align(7);
    char* str = reinterpret_cast<char*>(cursor);
    s.String(&str);
    assert(std::strcmp(str, "ab") == 0);
    auto insert = s.Insert();
    assert(reinterpret_cast<uint8_t*>(insert) == storage[5] + 8);
    *insert = storage[0];
    assert(s.Resolve((UINT64_C(5) << 32) | 9, true) == storage[0]);
    assert(s.Resolve((UINT64_C(5) << 32) | 1, false) == storage[5]);
    assert(s.Resolve(UINT64_C(0x1000000000) | 1, false) == storage[0] + 4);
    s.Push(1);
    auto temp = cursor;
    s.Load(true, cursor, 2);
    s.Pop();
    s.Push(1);
    assert(cursor == temp);
    s.Pop();
    s.Pop();
    s.EndAsset();
    s.Finish();
    assert(s.Consumed() == sizeof(data));
    Reject([&] { s.Pop(); });
    Reject([&] { s.EndAsset(); });
    Reject([&] { s.Push(8); });
    Reject([&] { s.Align(6); });
    Reject([&] { s.Resolve(UINT64_C(8) << 32, false); });
    Reject([&] { s.Resolve((UINT64_C(5) << 32) | 256, true); });
    Reject([&] { s.Resolve(UINT64_C(0x1000000000) | 1, false); });
    Reject([&] { s.Load(true, cursor, 1); });
    Reject([&] { s.Advance(257); });
    assert(!s.Contains(reinterpret_cast<uintptr_t>(storage[7]) + 256, 1));
    Streams bad(blocks, std::span(data, 6), cursor);
    bad.Load(true, cursor, 4);
    char* text = reinterpret_cast<char*>(cursor);
    Reject([&] { bad.String(&text); });
    Streams runtime(blocks, {}, cursor);
    runtime.Push(4);
    runtime.Load(true, cursor, 12);
    runtime.Pop();
    runtime.Finish();
    std::cout << "Replay streams: nested temporary rewind, packed/relative aliases, insertion, strings, runtime "
                 "zeroing and malformed inputs passed\n";
}
