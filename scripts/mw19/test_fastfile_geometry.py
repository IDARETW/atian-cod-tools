"""Test native fastfile loading through GLB output with synthetic geometry only."""
import argparse
import json
import struct
import subprocess
import zlib
from pathlib import Path
from fastfile_fixture import pack

ROOT = Path(__file__).resolve().parents[2]


def triangle(invalid_index=False):
    name = b'fixture/triangle\0'
    root = bytearray(96)
    struct.pack_into('<qQ', root, 0, -2, 1)
    shared_offset = ((16 + len(name) + 15) & ~15) + 192
    struct.pack_into('<QH', root, 48, (5 << 32) | (shared_offset + 1), 1)
    surface = bytearray(192)
    struct.pack_into('<HH', surface, 2, 3, 1)
    struct.pack_into('<I', surface, 40, 60)
    struct.pack_into('<II', surface, 48, 0xffffffff, 0xffffffff)
    struct.pack_into('<q', surface, 72, -2)
    struct.pack_into('<6f', surface, 144, 10, 20, 30, 1, 2, 4)
    shared = bytearray(66)
    identity = 512 | (512 << 10) | (256 << 20) | (3 << 30)
    for i in range(3):
        struct.pack_into('<I', shared, i * 20 + 16, identity)
        struct.pack_into('<H', shared, 60 + i * 2, i)
    struct.pack_into('<Q', shared, 20, 0x1fffff)
    struct.pack_into('<Q', shared, 40, 0x1fffff << 21)
    struct.pack_into('<H', shared, 32, 0x3c00)
    struct.pack_into('<H', shared, 54, 0x3c00)
    if invalid_index:
        struct.pack_into('<H', shared, 60, 3)
    body = struct.pack('<IIQIIQ', 0, 0, 0, 1, 0, 1) + struct.pack('<IIq', 8, 0, -2)
    body += root + name + surface + struct.pack('<qII', -2, len(shared), 0) + shared
    return pack(body, [0, 4096, 4096, 4096, 4096, 4096, 4096, 4096, 0, 0, 0])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('exe', type=Path)
    parser.add_argument('--acts', type=Path, default=ROOT / 'build/bin/Release/acts.exe')
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    args.out = args.out.resolve(); args.out.mkdir(parents=True, exist_ok=False)
    source = args.out / 'triangle.ff'; source.write_bytes(triangle())
    reader = [str(args.acts.resolve()), '--noUpdater', 'fastfile', '-r', 'mw19replay', '-g', str(args.exe.resolve())]

    def run(name, flags, file=source, success=True):
        output = args.out / name
        result = subprocess.run(reader + flags + ['-o', str(output), str(file)], cwd=ROOT, capture_output=True, timeout=60)
        (args.out / (name + '.log')).write_bytes(result.stdout + result.stderr)
        assert (result.returncode == 0) == success, (name, result.stdout[-2000:])
        folder = output / 'mw19replay' / file.stem
        return folder

    output = run('export', ['--geometry'])
    row = json.loads((output / 'assets.jsonl').read_text().strip())
    assert row['status'] == row['geometry_status'] == 'ok'
    assert (row['geometry_surfaces'], row['geometry_vertices'], row['geometry_triangles']) == (1, 3, 1)
    glb = (output / row['geometry_file']).read_bytes()
    assert len(glb) == row['geometry_bytes'] and zlib.crc32(glb) == row['geometry_crc32']
    assert struct.unpack_from('<III', glb) == (0x46546c67, 2, len(glb))
    json_size, kind = struct.unpack_from('<II', glb, 12)
    assert kind == 0x4e4f534a
    doc = json.loads(glb[20:20 + json_size])
    primitive = doc['meshes'][0]['primitives'][0]
    accessor = doc['accessors'][primitive['attributes']['POSITION']]
    view = doc['bufferViews'][accessor['bufferView']]
    start = 28 + json_size + view['byteOffset'] + accessor.get('byteOffset', 0)
    assert [struct.unpack_from('<3f', glb, start + i * view['byteStride']) for i in range(3)] == [(6, 16, 26), (14, 16, 26), (6, 24, 26)]
    tested = run('test', ['--test', '--geometry'])
    tested_row = json.loads((tested / 'assets.jsonl').read_text().strip())
    assert tested_row['geometry_crc32'] == row['geometry_crc32'] and 'geometry_file' not in tested_row
    assert {p.name for p in tested.rglob('*') if p.is_file()} == {'manifest.json', 'assets.jsonl'}
    broken = args.out / 'bad-index.ff'; broken.write_bytes(triangle(True))
    rejected = run('bad-index', ['--test', '--geometry'], broken, False)
    bad = json.loads((rejected / 'assets.jsonl').read_text().strip())
    assert bad['status'] == bad['geometry_status'] == 'failed'
    run('inventory-conflict', ['--test', '--geometry', '--noAssetDump'], success=False)
    (args.out / 'tests.json').write_text(json.dumps({'success': True, 'geometry': row, 'invalid_index': bad}, indent=2) + '\n')
    print('Native synthetic triangle -> GLB positions, metadata, CRC, report-only mode and bad-index rejection passed.')


if __name__ == '__main__':
    main()
