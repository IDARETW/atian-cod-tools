"""Generate tiny synthetic stream inputs or check report-only DDS capabilities.

An optional existing mw19xpaktest report describes one real entry. Its bytes
are independently decoded in memory during verification, never exported.
"""
import argparse
import json
import struct
import zlib
from pathlib import Path
from verify_xpak_sample import decode_sample


def create(directory, real_report=None, pool='image'):
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    if real_report:
        report = json.loads(Path(real_report).read_text())
        meta = report['metadata']
        assert report['success'] and meta['offset0'] == '0' and 'size1' not in meta
        size = report['payload_bytes']
        spec = dict(total_size=size, parts=[dict(key=report['key'], size=size)],
                    real_report=str(Path(real_report).resolve()))
        if pool == 'image':
            assert meta['type'] == 'image' and meta['levels'] == '1' and meta['format'] == '34'
            width, height = int(meta['width']), int(meta['height'])
            assert ((width + 3) // 4) * ((height + 3) // 4) * 8 == size
            spec.update(format=34, width=width, height=height, mips=1)
            spec['parts'][0].update(mips=1, width=width, height=height)
        else:
            assert meta['type'] == ('mesh' if pool == 'xmodelsurfs' else 'streamtree')
    else:
        spec = dict(format=6, width=8, height=8, total_size=352, mips=4,
                    parts=[dict(key=100, size=32, mips=2, width=2, height=2),
                           dict(key=101, size=96, mips=3, width=4, height=4),
                           dict(key=102, size=352, mips=4, width=8, height=8)])
        chunks = [b'C' * 16 + b'D' * 4 + b'\xcc' * 12, b'B' * 64, b'A' * 256]
        data, index = bytearray(), bytearray()
        for key, chunk in zip((100, 101, 102), chunks):
            block = bytearray(128)
            struct.pack_into('<3I', block, 0, 1, 0, len(chunk))
            block += chunk
            block += bytes(-len(block) % 128)
            index += struct.pack('<3Q', key, len(data), (1 << 63) | len(block))
            data += block
        header = bytearray(800)
        header[:8] = bytes.fromhex('4b41504900000d00')
        struct.pack_into('<Q', header, 16, len(header) + len(data) + len(index))
        struct.pack_into('<3Q', header, 0x138, 3, 800, len(data))
        struct.pack_into('<3Q', header, 0x150, 3, 800 + len(data), len(index))
        (directory / 'synthetic.xpak').write_bytes(header + data + index)
    spec['pool'] = pool
    if pool != 'image':
        spec['parts'] = spec['parts'][:1]
        spec['total_size'] = spec['parts'][0]['size']
    (directory / 'image.json').write_text(json.dumps(spec, indent=2) + '\n')


def verify(directory, spec_path, library=None, missing=False):
    directory = Path(directory)
    spec = json.loads(Path(spec_path).read_text())
    report = json.loads((directory / 'manifest.json').read_text())
    assert {p.name for p in directory.iterdir()} == {'manifest.json', 'progress.json', 'assets.jsonl'}
    assert report['test_only'] and report['complete'] and len(report['assets']) == 1
    asset = report['assets'][0]
    # Field traversal retains the runtime stream handle as external even when
    # the separate DDS payload check resolves the XPak bytes successfully.
    assert report['failures'] == 0 and report['partials'] == 1 and not report['success']
    assert asset['status'] == 'partial' and asset['field_read_errors'] == 0
    assert asset['external_payloads'] == 1 and asset['unresolved_pointers'] == asset['unresolved_unions'] == 0
    assert not any(key.endswith('_file') for key in asset)
    if missing:
        reason = 'streamed_image' if spec['pool'] == 'image' else 'stream_package_required'
        assert asset['payload_status'] == 'unavailable' and asset['payload_reason'] == reason
        assert 'payload_bytes' not in asset
        print(f"{spec['pool']} without package input explicitly unavailable; reports only.")
        return
    assert asset['payload_status'] == 'ok'
    if 'real_report' in spec:
        original = json.loads(Path(spec['real_report']).read_text())
        assert library
        pixels = decode_sample(original['archive'], library, original)
        words = [0] * 37
        if spec['pool'] == 'image':
            words[:8] = [0x20534444, 124, 0x81007, spec['height'], spec['width'], len(pixels), 0, 1]
            words[27] = 0x1000
            words[32:37] = [72, 3, 0, 1, 0]
    else:
        pixels = b'A' * 256 + b'B' * 64 + b'C' * 16 + b'D' * 4
        words = [0x20534444, 124, 0x2100f, 8, 8, 32, 0, 4] + [0] * 29
        words[27] = 0x401008
        words[32:37] = [28, 3, 0, 1, 0]
    words[19:22] = [32, 4, 0x30315844]
    if spec['pool'] == 'image': expected = struct.pack('<37I', *words) + pixels
    elif 'real_report' in spec: expected = pixels
    else: expected = b'C' * 16 + b'D' * 4 + b'\xcc' * 12
    assert asset['payload_bytes'] == len(expected) and asset['payload_crc32'] == zlib.crc32(expected)
    reads = asset['package_reads']
    assert len(reads) == len(spec['parts']) and all(r['status'] == 'ok' for r in reads)
    assert [r['key'] for r in reads] == [p['key'] for p in spec['parts']]
    assert sum(r['decoded_bytes'] for r in reads) == spec['total_size']
    print(f"XPak -> process header -> {spec['pool']} payload: {len(reads)} parts, {len(expected)} bytes, "
          f'CRC32 {zlib.crc32(expected):08x}; only three report files written.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['create', 'verify'])
    parser.add_argument('directory', type=Path)
    parser.add_argument('--real-report', type=Path)
    parser.add_argument('--spec', type=Path)
    parser.add_argument('--oodle', type=Path)
    parser.add_argument('--missing', action='store_true')
    parser.add_argument('--pool', choices=['image', 'streamkey', 'xmodelsurfs'], default='image')
    args = parser.parse_args()
    if args.action == 'create': create(args.directory, args.real_report, args.pool)
    else: verify(args.directory, args.spec, args.oodle, args.missing)
