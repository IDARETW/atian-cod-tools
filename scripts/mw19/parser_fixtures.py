"""Populated native parser regressions, with entirely synthetic asset data."""
import struct
from fastfile_fixture import pack


def make(records, version=0xff7):
    body = bytearray(struct.pack('<IIQIIQ', 0, 0, 0, len(records), 0, 1))
    body += b''.join(struct.pack('<IIq', kind, 0, -2) for kind, _ in records)
    body += b''.join(data for _, data in records)
    blocks = [0, 1024 * 1024, 65536, 65536, 1024 * 1024, 1024 * 1024, 65536, 65536, 0, 0, 0]
    result = pack(body, blocks)
    struct.pack_into('<I', result, 12, version)
    return result


def images():
    records = []
    for name, pixels in [('first', bytes.fromhex('ff0000ff00ff00ff0000ffffffffffff')),
                         ('second', bytes.fromhex('0102030405060708090a0b0c0d0e0f10'))]:
        root = bytearray(232)
        struct.pack_into('<q', root, 0, -2)
        struct.pack_into('<III', root, 20, 6, 0, 16)
        struct.pack_into('<4H', root, 36, 2, 2, 1, 1)
        root[48] = 1
        struct.pack_into('<q', root, 224, -2)
        records.append((19, root + name.encode() + b'\0' + pixels))
    return make(records)


def ddl():
    root = struct.pack('<qq', -2, -2)
    definition = bytearray(96)
    struct.pack_into('<q', definition, 0, -2)
    struct.pack_into('<qI', definition, 40, -2, 1)
    structure = bytearray(56)
    struct.pack_into('<qIIq', structure, 0, -2, 96, 2, -2)
    members = bytearray(128)
    for i in range(2):
        off = i * 64
        struct.pack_into('<qi', members, off, -2, i)
        struct.pack_into('<q', members, off + 16, -2 if i == 0 else 0)
        struct.pack_into('<iiii', members, off + 24, 32 * (i + 1), 0, 32 * i, i + 1)
        struct.pack_into('<i', members, off + 52, i + 3)
    return make([(57, root + b'fixture.ddl\0' + definition + b'definition\0' + structure + b'root\0' +
                  members + b'first\0\xa5second\0')])


def world_bounds():
    world = bytearray(17808)
    struct.pack_into('<q', world, 0, -2)
    struct.pack_into('<I', world, 200, 2)
    struct.pack_into('<q', world, 248, -2)
    bounds = bytearray(112)
    for i in range(2):
        struct.pack_into('<6f', bounds, i * 56, *(float(i * 10 + n) for n in range(1, 7)))
        bounds[i * 56 + 24:i * 56 + 56] = bytes(range(i * 32, (i + 1) * 32))
    return make([(31, world + b'world_bounds\0' + bounds)])


def legacy_worlds():
    com = bytearray(152)
    struct.pack_into('<q', com, 0, -2)
    struct.pack_into('<I', com, 48, 2)
    struct.pack_into('<q', com, 56, -2)
    lights = bytearray(2 * 344)
    struct.pack_into('<q', lights, 336, -2)
    struct.pack_into('<q', lights, 344 + 336, -2)
    lights[0] = 17; lights[344] = 29
    scriptable = bytearray(112)
    struct.pack_into('<q', scriptable, 0, -2)
    struct.pack_into('<I', scriptable, 92, 2)
    struct.pack_into('<q', scriptable, 96, -2)
    world = bytearray(17744)
    struct.pack_into('<q', world, 0, -2)
    struct.pack_into('<I', world, 24, 1)
    struct.pack_into('<q', world, 15464, -2)
    struct.pack_into('<q', world, 15472, -2)
    mesh = bytearray(96)
    struct.pack_into('<I', mesh, 0, 3)
    struct.pack_into('<q', mesh, 8, -2)
    struct.pack_into('<I', mesh, 16, 3)
    struct.pack_into('<q', mesh, 24, -2)
    vertices = b''.join(struct.pack('<8f', *p, 0, 0, 0, 0, 0) for p in [(0,0,0),(1,0,0),(0,1,0)])
    frustum = bytearray(48)
    for off, count in [(0,1),(16,3),(32,3)]:
        struct.pack_into('<I', frustum, off, count); struct.pack_into('<q', frustum, off+8, -2)
    records = [(24, com + b'world\0' + lights + b'point\0spot\0'),
               (63, scriptable + b'scriptable\0' + bytes(16)),
               (31, world + b'graphics\0' + mesh + struct.pack('<3H',0,1,2) + vertices + frustum +
                struct.pack('<4f',0,0,1,0) + struct.pack('<3H',0,1,2) +
                b''.join(struct.pack('<3f',*p) for p in [(0,0,0),(1,0,0),(0,1,0)]))]
    return make(records, 0xfda)
