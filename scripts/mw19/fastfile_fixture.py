"""Small synthetic Replay fastfiles for native-loader tests; no game data."""
import argparse
import json
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def pack(body, blocks):
    chunks = bytearray(b'\x02IWC' + struct.pack('<I', len(body)) + b'\x00\x00\x01\x01')
    for start in range(0, len(body), 65536):
        part = body[start:start + 65536]
        chunks += struct.pack('<III', len(part), len(part), 0) + part
        chunks += bytes(-len(chunks) & 3)
    header = bytearray(136)
    header[:8] = b'IWffc100'
    struct.pack_into('<II', header, 8, 11, 0xff7)
    struct.pack_into('<I', header, 20, len(chunks))
    struct.pack_into('<QQ', header, 24, 136 + len(chunks), len(body))
    struct.pack_into('<11Q', header, 48, *blocks)
    return header + chunks


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    profile = json.loads((ROOT / 'config/data/mw19/schema.json').read_text())['profiles']['replay-1.20']
    rows = []
    for pool in profile['pools']:
        if not pool.get('pointer_loader_rva'):
            continue
        # Each file deliberately isolates one generated loader. This does not
        # prove populated field traversal; separate populated fixtures do that.
        body = bytearray(struct.pack('<IIQIIQ', 0, 0, 0, 1, 0, 1))
        body += struct.pack('<IIq', pool['id'], 0, -2)
        root = bytearray(pool['size'])
        struct.pack_into('<q', root, pool['name_offset'], -2)
        body += root + ('fixture/' + pool['name']).encode() + b'\0'
        # Plenty of room for zero-count runtime alignment; the root header is
        # outside the eight native streams and reserves no stream-zero bytes.
        blocks = [0, max(pool['size'] + 128, 4096), 4096, 4096, 4096, 65536, 4096, 4096, 0, 0, 0]
        path = args.output / (pool['name'] + '.ff')
        path.write_bytes(pack(body, blocks))
        rows.append({'pool': pool['name'], 'id': pool['id'], 'file': path.name})
    (args.output / 'fixtures.json').write_text(json.dumps(rows, indent=2) + '\n')
    print(f'Created {len(rows)} isolated synthetic Replay loader fixtures')


if __name__ == '__main__':
    main()
