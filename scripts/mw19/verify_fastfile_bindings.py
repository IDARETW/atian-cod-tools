"""Verify the checked-in Replay adapter bindings against the owner's PE.

Read-only, standard-library verification. Does not need IDA, SQLite, game
source, or a disassembler, and never executes the supplied executable.
"""
import argparse
import hashlib
import json
import re
import struct
from pathlib import Path
from verify_replay import PE

ROOT = Path(__file__).resolve().parents[2]


def verify(exe):
    pe = PE(exe)
    text = (ROOT / 'src/core/acts/tools/mw19/mw19_replay_bindings.hpp').read_text()
    profile = json.loads((ROOT / 'config/data/mw19/schema.json').read_text())['profiles']['replay-1.20']
    records = re.findall(r'\{\s*(\d+),\s*(0x[0-9a-f]+),\s*(0x[0-9a-f]+),\s*'
                         r'(0x[0-9a-f]+),\s*\{([^}]+)\}\s*\}', text)
    enabled = {p['id']: p for p in profile['pools'] if p.get('pointer_loader_rva')}
    assert len(records) == len(enabled) == 111
    assert {int(row[0]) for row in records} == set(enabled)
    for ordinal, loader, slot, link, signature in records:
        ordinal, loader, slot, link = int(ordinal), int(loader, 16), int(slot, 16), int(link, 16)
        expected = bytes(int(b, 16) for b in re.findall(r'0x[0-9a-f]+', signature))
        assert len(expected) == 16 and pe.read(loader, 16) == expected, ('signature', ordinal)
        assert loader == enabled[ordinal]['pointer_loader_rva'], ('catalog loader', ordinal)
        prologue = pe.read(loader, 24)
        at = prologue.index(bytes.fromhex('48 8b 15'))
        assert loader + at + 7 + struct.unpack_from('<i', prologue, at + 3)[0] == slot, ('header slot', ordinal)
        code = pe.read(loader, 0x400)
        # A direct call to the dedicated Link wrapper must exist in this
        # bounded pointer-loader window. Entry signatures and catalog RVAs
        # independently tie the window to this exact Replay build.
        assert any(code[i] == 0xe8 and loader + i + 5 + struct.unpack_from('<i', code, i + 1)[0] == link
                   for i in range(len(code) - 4)), ('Link call', ordinal)
    print(json.dumps(dict(bindings=111, success=True, sha256=hashlib.sha256(pe.data).hexdigest())))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('exe', type=Path)
    verify(parser.parse_args().exe)
