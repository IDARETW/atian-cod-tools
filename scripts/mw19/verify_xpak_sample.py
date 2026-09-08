"""Independently verify a single-block real XPak test report, without asset files.

Uses the supplied local Oodle library through ctypes. Reads only the header,
index and selected entry; payload bytes stay in memory.
"""
import ctypes
import json
import struct
import sys
import zlib
from pathlib import Path


def decode_sample(archive, library, report):
    assert report['test_only'] and report['complete'] and report['success']
    assert 0 < report['payload_bytes'] <= 128 * 1024 * 1024
    with Path(archive).open('rb') as file:
        header = file.read(800)
        assert header[:8] == bytes.fromhex('4b41504900000d00')
        data_offset = struct.unpack_from('<Q', header, 0x140)[0]
        count, offset, size = struct.unpack_from('<3Q', header, 0x150)
        assert size == count * 24 and size <= 64 * 1024 * 1024
        file.seek(offset)
        entries = struct.iter_unpack('<3Q', file.read(size))
        selected = next(e for e in entries if e[0] == report['key'])
        key, relative, packed_size = selected
        assert packed_size >> 63 and packed_size & ((1 << 63) - 1) == report['stored_bytes']
        file.seek(data_offset + relative)
        packet = file.read(report['stored_bytes'])
    count, destination, command = struct.unpack_from('<3I', packet)
    assert count == 1 and destination == 0 and command >> 24 in (0, 6, 7), 'This verifier intentionally covers one raw/Oodle block'
    size = command & 0xffffff
    assert size <= len(packet) - 128
    if command >> 24 == 0:
        decoded = packet[128:128 + size]
        assert len(decoded) == report['payload_bytes'] and zlib.crc32(decoded) == report['payload_crc32']
        return decoded
    source = ctypes.create_string_buffer(packet[128:128 + size])
    output = ctypes.create_string_buffer(report['payload_bytes'])
    library = ctypes.WinDLL(str(Path(library).resolve()))
    decode = library.OodleLZ_Decompress
    decode.restype = ctypes.c_ssize_t
    decode.argtypes = [ctypes.c_void_p, ctypes.c_ssize_t, ctypes.c_void_p, ctypes.c_ssize_t,
                      ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_void_p, ctypes.c_ssize_t,
                      ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_ssize_t, ctypes.c_int]
    written = decode(source, size, output, len(output), 1, 0, 0, None, 0, None, None, None, 0, 3)
    assert written == report['payload_bytes']
    checksum = zlib.crc32(output.raw)
    assert checksum == report['payload_crc32']
    assert not any(k.endswith('_file') for k in report)
    return output.raw


def verify(archive, library, report):
    report = json.loads(Path(report).read_text())
    decoded = decode_sample(archive, library, report)
    key, written, checksum = report['key'], len(decoded), zlib.crc32(decoded)
    print(f'Independent Oodle decode matched key {key:016x}: {written} bytes, CRC32 {checksum:08x}; no asset files written.')


if __name__ == '__main__': verify(*sys.argv[1:])
