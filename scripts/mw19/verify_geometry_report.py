"""Validate the tiny geometry fixture in RAM, then compare CLI report checksums."""
import json
import math
import struct
import subprocess
import sys
import zlib
from pathlib import Path


def inspect_glb(data):
    magic, version, length, json_size, json_kind = struct.unpack_from('<5I', data)
    assert (magic, version, length, json_kind) == (0x46546c67, 2, len(data), 0x4e4f534a)
    assert json_size % 4 == 0
    doc = json.loads(data[20:20 + json_size])
    binary_size, binary_kind = struct.unpack_from('<2I', data, 20 + json_size)
    binary = data[28 + json_size:]
    assert binary_kind == 0x4e4942 and binary_size == len(binary) == doc['buffers'][0]['byteLength']
    assert len(doc['meshes'][0]['primitives']) == 1
    primitive = doc['meshes'][0]['primitives'][0]

    def values(index):
        a = doc['accessors'][index]
        view = doc['bufferViews'][a['bufferView']]
        width = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3}[a['type']]
        fmt = '<' + {5126: 'f', 5123: 'H'}[a['componentType']] * width
        size = struct.calcsize(fmt)
        start = view.get('byteOffset', 0) + a.get('byteOffset', 0)
        stride = view.get('byteStride', size)
        assert view.get('byteOffset', 0) % 4 == 0
        assert a.get('byteOffset', 0) + stride * (a['count'] - 1) + size <= view['byteLength']
        assert view.get('byteOffset', 0) + view['byteLength'] <= len(binary)
        return [struct.unpack_from(fmt, binary, start + i * stride) for i in range(a['count'])]

    attrs = primitive['attributes']
    positions = values(attrs['POSITION'])
    assert positions == [(6, 16, 26), (14, 16, 26), (6, 24, 26)]
    assert values(attrs['TEXCOORD_0']) == [(0, 0), (1, 0), (0, 1)]
    assert values(primitive['indices']) == [(0,), (1,), (2,)]
    for x, y, z in values(attrs['NORMAL']):
        assert math.isclose(x*x + y*y + z*z, 1, abs_tol=1e-6) and z > 0.999
    assert doc['accessors'][attrs['POSITION']]['min'] == [6, 16, 26]
    assert doc['accessors'][attrs['POSITION']]['max'] == [14, 24, 26]
    assert 'skins' not in doc and doc['extras']['scope'] == 'base surface geometry'
    assert doc['nodes'][0]['scale'] == [0.0254]*3
    assert math.isclose(doc['nodes'][0]['rotation'][0], -math.sqrt(0.5))
    return doc


def verify(report, executable, negative=False):
    data = subprocess.check_output([str(executable), '--emit-glb'])
    inspect_glb(data)
    report = Path(report)
    assert {p.name for p in report.iterdir()} == {'manifest.json', 'assets.jsonl', 'progress.json'}
    manifest = json.loads((report / 'manifest.json').read_text())
    assert manifest['complete'] and manifest['test_only']
    assert manifest['partials'] == 0 and manifest['failures'] == int(negative)
    assets = manifest['assets']
    assert len(assets) == (2 if negative else 1)
    first = assets[0]
    assert first['status'] == first['payload_status'] == first['geometry_status'] == 'ok'
    assert first['geometry_bytes'] == len(data) and first['geometry_crc32'] == zlib.crc32(data)
    assert (first['geometry_surfaces'], first['geometry_vertices'], first['geometry_triangles']) == (1, 3, 1)
    if negative:
        bad = assets[1]
        assert bad['geometry_status'] == bad['status'] == 'failed'
        assert bad['payload_status'] == bad['field_status'] == 'ok'
        assert 'Triangle index' in bad['geometry_error'] and 'geometry_bytes' not in bad
    assert not any(key.endswith('_file') for a in assets for key in a)
    print(f'Geometry verified in memory: {len(data)} GLB bytes, CRC32 {zlib.crc32(data):08x}; '
          f'{len(assets)} synthetic process samples; no asset files written.')


if __name__ == '__main__':
    verify(sys.argv[1], sys.argv[2], '--negative' in sys.argv[3:])
