"""Verify every mapped Replay pool against an on-disk PE without executing it."""
import argparse
import hashlib
import json
import struct
from pathlib import Path


class PE:
    def __init__(self, path):
        self.data = Path(path).read_bytes()
        self.path = str(path)
        if self.data[:2] != b'MZ':
            raise ValueError('Not a PE executable')
        pe = self.u32(0x3c)
        if self.data[pe:pe + 4] != b'PE\0\0':
            raise ValueError('Invalid PE signature')
        machine, count = struct.unpack_from('<HH', self.data, pe + 4)
        opt_size = struct.unpack_from('<H', self.data, pe + 20)[0]
        opt = pe + 24
        if machine != 0x8664 or struct.unpack_from('<H', self.data, opt)[0] != 0x20b:
            raise ValueError('Expected a 64-bit PE image')
        self.base = struct.unpack_from('<Q', self.data, opt + 24)[0]
        self.image_size = self.u32(opt + 56)
        self.sections = []
        for i in range(count):
            off = opt + opt_size + 40 * i
            name, vsize, rva, raw_size, raw = struct.unpack_from('<8sIIII', self.data, off)
            if raw + raw_size > len(self.data):
                raise ValueError('Section extends beyond file')
            self.sections.append((rva, max(vsize, raw_size), raw, raw_size))

    def u32(self, offset):
        return struct.unpack_from('<I', self.data, offset)[0]

    def read(self, rva, length):
        for start, size, raw, raw_size in self.sections:
            if start <= rva and rva + length <= start + size:
                delta = rva - start
                available = min(length, max(0, raw_size - delta))
                return self.data[raw + delta:raw + delta + available] + bytes(length - available)
        raise ValueError(f'Unmapped RVA 0x{rva:x} + 0x{length:x}')

    def string(self, pointer):
        if not pointer:
            return None
        out = bytearray()
        for i in range(4096):
            b = self.read(pointer - self.base + i, 1)
            if b == b'\0':
                return out.decode('utf-8')
            out += b
        raise ValueError('Unterminated PE string')


def verify(exe, schema):
    pe = PE(exe)
    p = schema['profiles']['replay-1.20']
    t = p['table_rvas']
    errors = []
    rows = []
    signatures = {
        0xF5E5B0: '4863c1488d1576db4901488b14c248ffe2',
        0xF5E740: '4863c1488d0c40488d053a68990a488d14c8488b04c84885c07407488b0848890ac3c3',
    }
    for rva, hex_bytes in signatures.items():
        expected = bytes.fromhex(hex_bytes)
        if pe.read(rva, len(expected)) != expected:
            errors.append(f'Allocator code mismatch at 0x{rva:x}')
    if 'technique_set_layout' in p:
        for rva, code in [(0xe0d308, '488b7338'), (0xe0d380, 'f3490fb84cc118'),
                          (0xe0d38d, '4883f804'), (0xe0d398, '4c8d04fd00000000'),
                          (0xe0d3bd, 'e81efeffff')]:
            if pe.read(rva, len(code) // 2) != bytes.fromhex(code):
                errors.append(f'Technique set binding mismatch at 0x{rva:x}')
        layout = p['technique_set_layout']
        if (layout['mask_offset'], layout['mask_words'], layout['mask_word_bytes'],
            layout['masked_techniques_offset']) != (24, 4, 8, 56):
            errors.append('Incorrect Replay technique mask layout')
    if 'cell_tree_layout' in p:
        for rva, code in [(0xd96bc1, '41b818000000'), (0xd96c10, '49c1e002'),
                          (0xd96c4d, '48894710'), (0xd96c68, '4c8d04fd00000000'),
                          (0xd908ed, '41b808000000'), (0xd90943, 'e868204200'),
                          (0xd90953, '4c8d047f49c1e004')]:
            if pe.read(rva, len(code) // 2) != bytes.fromhex(code):
                errors.append(f'Cell tree binding mismatch at 0x{rva:x}')
        layout = p['cell_tree_layout']
        if (layout['owner_size'], layout['count_record_size'], layout['tree_record_size'],
            layout['aabb_record_size']) != (24, 4, 8, 48):
            errors.append('Incorrect Replay cell tree record size')
    if 'image_layout' in p:
        image = p['image_layout']
        for key, table in [('dxgi_table_rva', 'dxgi_formats'), ('pixel_bytes_table_rva', 'pixel_bytes')]:
            expected = struct.pack('<' + 'I' * len(image[table]), *image[table])
            if pe.read(image[key], len(expected)) != expected:
                errors.append(f'Image table mismatch: {table}')
        # Native upload clears image.pixels (+0xe0) after creating textureId.
        if pe.read(0x193888D, 7) != bytes.fromhex('4889b7e0000000'):
            errors.append('Image CPU pointer clearing instruction mismatch')
        # Native stream count +50 and totalSize minus inclusive part size.
        for rva, code in [(0x139f7b6, '0fb64832'),
                          (0x13b331c, '418b401c488d0c89418b54c858c1ea042bc24903c1')]:
            if pe.read(rva, len(code) // 2) != bytes.fromhex(code):
                errors.append(f'Image stream layout instruction mismatch at 0x{rva:x}')
        if image.get('streamed_part_count_offset') != 50:
            errors.append('Incorrect Replay image stream count offset')
    if 'stream_buffer_layout' in p:
        for rva, code in [(0xe34061, 'f6403d02'), (0xe2afad, 'f6400c01'),
                          (0x13b33a5, '488b018b4038c3'), (0x13b33ac, '488b01488b48308b4108c3'),
                          (0x13ad78f, 'e8dc1cbbff4883c010'), (0x13ad79e, 'e80d1cbbff4883c008'),
                          (0x13acbdd, 'c6433c0048c7433000000000')]:
            if pe.read(rva, len(code) // 2) != bytes.fromhex(code):
                errors.append(f'Stream buffer binding mismatch at 0x{rva:x}')
    if 'mesh_geometry_layout' in p:
        for rva, code in [(0x1bb743d, '4881c7c0000000'),
                          (0x1bb7397, '8b4f248d04b64903c8c1e002488d9790000000'),
                          (0x1bb74b1, '8b4728448d04764803c84881c790000000'),
                          (0xec15dc, '488d7614'),
                          (0x1992d60, '48c1e915'), (0x1992d84, '48c1e82a')]:
            if pe.read(rva, len(code) // 2) != bytes.fromhex(code):
                errors.append(f'Mesh geometry binding mismatch at 0x{rva:x}')
        if pe.read(0x2598d40, 4) != struct.pack('<f', 1.0 / 2097151):
            errors.append('Packed position reciprocal mismatch')
        layout = p['mesh_geometry_layout']
        if (layout['surface_size'], layout['vertex_stride'], layout['triangle_stride']) != (192, 20, 6):
            errors.append('Incorrect Replay surface or geometry stride')
    if 'sound_bank_layout' in p:
        for rva, code in [(0xe39f55, '488d96f0010000'), (0xe39f8b, 'e8c0a1ffff'),
                          (0x1b859dd, '486bd22c'),
                          (0x1b85b02, '81f932555823'),
                          (0x1b85b22, '448b4a044183f90a'),
                          (0x1b85b4c, '448b4a1c4183f910'),
                          (0x1b85b76, '448b4a084183f92c'),
                          (0x1b86094, '41f6463d02'),
                          (0x1b86114, '488b4e28'),
                          (0x1b863dc, '488b5014')]:
            if pe.read(rva, len(code) // 2) != bytes.fromhex(code):
                errors.append(f'Sound bank binding mismatch at 0x{rva:x}')
    for pool in p['pools']:
        i = pool['id']
        name = pe.string(struct.unpack('<Q', pe.read(t['names'] + i * 8, 8))[0])
        size = struct.unpack('<I', pe.read(t['sizes'] + i * 4, 4))[0]
        capacity = struct.unpack('<I', pe.read(t['capacity'] + i * 4, 4))[0]
        alignment = struct.unpack('<I', pe.read(t['alignment'] + i * 4, 4))[0]
        default = pe.string(struct.unpack('<Q', pe.read(0x2457DB0 + i * 8, 8))[0])
        allocator = struct.unpack('<Q', pe.read(t['allocators'] + i * 8, 8))[0]
        storage = struct.unpack('<Q', pe.read(t['pool_pointers'] + i * 8, 8))[0]
        actual = dict(id=i, name=name, size=size, capacity=capacity, alignment=alignment,
                      default_asset=default, allocator_rva=allocator - pe.base if allocator else None,
                      storage_rva=storage - pe.base if storage else None)
        mismatches = [key for key in ('name', 'size', 'capacity', 'alignment', 'default_asset', 'allocator_rva') if actual[key] != pool[key]]
        actual['verified'] = not mismatches
        if mismatches:
            errors.append(f'Pool {i}: mismatched {mismatches}')
        rows.append(actual)
    return dict(schema_version=1, executable=pe.path, sha256=hashlib.sha256(pe.data).hexdigest(),
                image_base=pe.base, image_size=pe.image_size, pools=rows, errors=errors, success=not errors)


def main():
    root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('exe', type=Path)
    parser.add_argument('--schema', type=Path, default=root / 'config/data/mw19/schema.json')
    parser.add_argument('--out', type=Path, default=root / 'research/mw19/replay_verification.json')
    args = parser.parse_args()
    report = verify(args.exe, json.loads(args.schema.read_text(encoding='utf-8')))
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(dict(verified_pools=sum(p['verified'] for p in report['pools']),
                          success=report['success'], errors=report['errors'], sha256=report['sha256'])))
    raise SystemExit(0 if report['success'] else 1)


if __name__ == '__main__':
    main()
