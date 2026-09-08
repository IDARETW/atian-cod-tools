"""Recover root loaders from native calls, without trusting transferred names.

The SQLite export supplies pseudocode by exact address. The executable supplies
call targets, and Load_Stream's byte count must equal the independently verified
pool size. Raw decompilation remains in the ignored research directory.
"""
import argparse
import json
import re
import sqlite3
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'build/python'))
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_IMM
from verify_replay import PE


def recover(exe, sqlite):
    pe = PE(exe)
    connection = sqlite3.connect(Path(sqlite).as_uri() + '?mode=ro', uri=True)
    schema = json.loads((ROOT / 'config/data/mw19/schema.json').read_text())
    pools = schema['profiles']['replay-1.20']['pools']
    pointers = json.loads((ROOT / 'research/mw19/native_asset_loaders.json').read_text())
    dis = Cs(CS_ARCH_X86, CS_MODE_64)
    dis.detail = True
    result = {}
    for name, pointer in pointers.items():
        row = connection.execute('select size from functions where address=?', (str(pointer['va']),)).fetchone()
        candidates = []
        for ins in dis.disasm(pe.read(pointer['va'] - pe.base, row[0]), pointer['va']):
            if ins.mnemonic != 'call' or ins.operands[0].type != X86_OP_IMM:
                continue
            address = ins.operands[0].imm
            row2 = connection.execute('select size,pseudocode from functions where address=?', (str(address),)).fetchone()
            if not row2:
                continue
            size, code = row2
            if not code or 'sub_140D8DA80(' not in code:
                continue
            match = re.search(r'sub_1411B2A20\([^\n]*a2: (\w+), a3: (\d+)\)', code)
            if not match or int(match[2]) != pools[pointer['id']]['size']:
                continue
            candidates.append(dict(id=pointer['id'], pointer_loader_rva=pointer['va'] - pe.base,
                                   loader_rva=address - pe.base, root_global=int(match[1].split('_')[-1], 16),
                                   code_size=size, code=code))
        if len(candidates) != 1:
            raise RuntimeError(f'{name}: {len(candidates)} root loader candidates')
        result[name] = candidates[0]
    dest = ROOT / 'research/mw19/native_root_loaders.json'
    dest.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({'root_loaders': len(result), 'pools_without_loaders': [p['name'] for p in pools if p['name'] not in result]}))


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--exe', required=True)
    parser.add_argument('--sqlite', required=True)
    args = parser.parse_args()
    recover(args.exe, args.sqlite)
