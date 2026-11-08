"""GPU buffer records from the x64-zt Replay layout corrections.

Source: x64-zt tools/iw8_layouts.txt at commit caf5b0f (git -C E:\\mw124\\x64-zt log -1 --format=%h).
Offsets are bytes. The nine records and the five aliases that name them are copied from that file;
each record's evidence is the comment block directly above it in the file.

apply(types, profile) writes the Replay:: records into a schema type table and sets
profile['type_overrides'] for the aliases. It returns a list of problems (empty when clean).
Run `python -I scripts/mw19/replay_layouts_x64zt_gpu.py --check` to verify the result.
"""
import argparse
import copy
import json
import os
import re

SOURCE_LAYOUTS = r'E:\mw124\x64-zt\tools\iw8_layouts.txt'
SCHEMA = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..',
                      'config', 'data', 'mw19', 'schema.json')
PROFILE = 'replay-1.20'

SCALAR_SIZES = {
    'char': 1, 'signed char': 1, 'unsigned char': 1, '__int8': 1, 'unsigned __int8': 1, 'bool': 1,
    'short': 2, 'unsigned short': 2, '__int16': 2, 'unsigned __int16': 2, 'wchar_t': 2,
    'int': 4, 'unsigned int': 4, 'long': 4, 'unsigned long': 4, '__int32': 4, 'unsigned __int32': 4, 'float': 4,
    'double': 8, '__int64': 8, 'unsigned __int64': 8, 'long long': 8, 'unsigned long long': 8,
}

# Offsets in bytes, member order as in iw8_layouts.txt. Member types are spelled as in the source.
RECORDS = [
  {
    'name': 'Replay::GfxBufferResource',
    'kind': 'struct',
    'size': 32,
    'members': [
        (0, 'resource', 'ID3D12Resource *'),
        (8, 'gpuAddress', 'unsigned __int64'),
        (16, 'allocation', 'unsigned __int64'),
        (24, 'size', 'unsigned int'),
    ],
    'evidence': 'Null record: q_R_AssignNullShaderBufferView (RVA 0x18BC9B0) clears qwords 0, 8, 16 and dword 24.',
  },
  {
    'name': 'Replay::GfxShaderBufferView',
    'kind': 'struct',
    'size': 16,
    'members': [
        (0, 'resource', 'ID3D12Resource *'),
        (8, 'view', 'unsigned int'),
    ],
    'evidence': 'Null view: R_AssignNullShaderBufferView (RVA 0x18BCA00) writes {0, 1}.',
  },
  {
    'name': 'Replay::GfxShaderBufferRWView',
    'kind': 'struct',
    'size': 32,
    'members': [
        (0, 'resource', 'ID3D12Resource *'),
        (8, 'gpuAddress', 'unsigned __int64'),
        (16, 'allocation', 'unsigned __int64'),
        (24, 'view', 'unsigned int'),
    ],
    'evidence': '',
  },
  {
    'name': 'Replay::GfxWrappedBaseBuffer',
    'kind': 'struct',
    'size': 48,
    'members': [
        (0, 'buffer', 'Replay::GfxBufferResource'),
        (32, 'view', 'Replay::GfxShaderBufferView'),
    ],
    'evidence': '',
  },
  {
    'name': 'Replay::GfxWrappedBuffer',
    'kind': 'struct',
    'size': 64,
    'members': [
        (0, 'base', 'Replay::GfxWrappedBaseBuffer'),
        (48, 'data', 'void *'),
        (56, 'flags', 'unsigned __int8'),
    ],
    'evidence': 'R_CreateGfxWrappedBuffer (RVA 0x18DABF0) stores the created resource at +0x30, flags at +0x38 and builds the read-write view at +0x40.',
  },
  {
    'name': 'Replay::GfxWrappedRWBuffer',
    'kind': 'struct',
    'size': 96,
    'members': [
        (0, 'buffer', 'Replay::GfxWrappedBuffer'),
        (64, 'rwView', 'Replay::GfxShaderBufferRWView'),
    ],
    'evidence': '',
  },
  {
    'name': 'Replay::GfxIndexBuffer',
    'kind': 'struct',
    'size': 32,
    'members': [
        (0, 'resource', 'ID3D12Resource *'),
        (8, 'gpuAddress', 'unsigned __int64'),
        (16, 'allocation', 'unsigned __int64'),
        (24, 'size', 'unsigned int'),
    ],
    'evidence': 'Load_GfxIndexBuffer (RVA 0xDD1770) and the vertex buffer loader (RVA 0xDD1790) load 0x20. q_Load_IndexBufferView (RVA 0x18D9F10) clears a null record with the 32-byte null routine.',
  },
  {
    'name': 'Replay::GfxVertexBuffer',
    'kind': 'struct',
    'size': 32,
    'members': [
        (0, 'resource', 'ID3D12Resource *'),
        (8, 'gpuAddress', 'unsigned __int64'),
        (16, 'allocation', 'unsigned __int64'),
        (24, 'size', 'unsigned int'),
    ],
    'evidence': '',
  },
  {
    'name': 'Replay::GfxConstantBuffer',
    'kind': 'struct',
    'size': 56,
    'members': [
        (0, 'desc', 'GfxConstantBufferDesc'),
        (16, 'buffer', 'Replay::GfxBufferResource'),
        (48, 'view', 'unsigned __int64'),
    ],
    'evidence': 'q_Load_MaterialConstantBufferDef (RVA 0xE0C5F0) loads 0x110; q_Load_MaterialConstantBufferTable (RVA 0x194B9D0) creates the four stage buffers at +0x30, +0x68, +0xA0 and +0xD8 (0x38 apart). The members after the game-test descriptor are runtime only (inferred).',
  },
]

ALIASES = {
    'GfxBufferResource': 'Replay::GfxBufferResource',
    'GfxShaderBufferRWView': 'Replay::GfxShaderBufferRWView',
    'GfxIndexBuffer': 'Replay::GfxIndexBuffer',
    'GfxVertexBuffer': 'Replay::GfxVertexBuffer',
    'GfxConstantBuffer': 'Replay::GfxConstantBuffer',
}


def _clean(name):
    name = ' '.join(name.split())
    for prefix in ('const ', 'volatile '):
        while name.startswith(prefix):
            name = name[len(prefix):]
    return name


def _size(types, overrides, type_name):
    """Byte size of a member type, or None when the schema does not define it."""
    name = _clean(type_name)
    if name.endswith('*') or '(*)' in name:
        return 8
    if name in SCALAR_SIZES:
        return SCALAR_SIZES[name]
    entry = types.get(overrides.get(name, name))
    return None if entry is None else entry['size']


def apply(types, profile):
    """Write the GPU records into types and the aliases into profile['type_overrides']."""
    overrides = profile.setdefault('type_overrides', {})
    problems = []
    for record in RECORDS:
        types[record['name']] = {'name': record['name'], 'kind': record['kind'],
                                 'size': record['size'], 'members': []}
    for record in RECORDS:
        name, size = record['name'], record['size']
        members = []
        for offset, member, type_name in record['members']:
            width = _size(types, overrides, type_name)
            if width is None:
                problems.append('%s.%s: type %r is not in the schema' % (name, member, type_name))
                width = 0
            members.append({'name': member, 'offset_bits': offset * 8, 'size_bits': width * 8, 'type': type_name})
        members.sort(key=lambda m: m['offset_bits'])
        end = 0
        for m in members:
            if m['offset_bits'] < end:
                problems.append('%s.%s: overlaps the previous member' % (name, m['name']))
            end = max(end, m['offset_bits'] + m['size_bits'])
        if end > size * 8:
            problems.append('%s: members end at %d, past the size %d' % (name, end // 8, size))
        types[name]['members'] = members
    for base, target in ALIASES.items():
        if target in types:
            overrides[base] = target
        else:
            problems.append('alias %s -> %s: no such type' % (base, target))
    return problems


def _children(types, overrides, type_name):
    name = _clean(type_name)
    resolved = overrides.get(name, name)
    entry = types.get(resolved)
    if entry is None:
        return [resolved[:-1].strip()] if resolved.endswith('*') else []
    kind = entry.get('kind')
    if kind in ('struct', 'union'):
        return [m['type'] for m in entry.get('members', [])]
    if kind == 'pointer':
        return [entry['target']] if entry.get('target') else []
    if kind == 'array':
        return [entry['element']]
    return []


def _reachable(types, overrides, root):
    """Every schema type name reached from root through struct members, pointers and arrays."""
    seen, stack = set(), [root]
    while stack:
        name = _clean(stack.pop())
        resolved = overrides.get(name, name)
        if resolved in seen:
            continue
        seen.add(resolved)
        stack.extend(_children(types, overrides, name))
    return seen


def _read_source(path):
    """Minimal reader for tools/iw8_layouts.txt: its struct records and alias lines."""
    records, aliases = {}, {}
    current = None
    with open(path, encoding='utf-8') as handle:
        for raw in handle:
            line = raw.split('#', 1)[0].rstrip()
            if not line.strip():
                continue
            if not raw[0].isspace():
                words = line.split()
                current = None
                if words[0] in ('struct', 'union') and len(words) == 3:
                    current = words[1]
                    records[current] = {'size': int(words[2], 0), 'members': []}
                elif words[0] == 'alias' and len(words) == 3:
                    aliases[words[1]] = words[2]
                continue
            match = re.match(r'\s+(\d+|0x[0-9A-Fa-f]+)\s+(\w+)\s*:\s*(.+)$', line)
            if match and current is not None:
                records[current]['members'].append((int(match[1], 0), match[2], match[3].strip()))
    return records, aliases


def _check(args):
    family = {r['name'] for r in RECORDS}
    bases = {n.split('::')[-1] for n in family}
    with open(args.schema, encoding='utf-8') as handle:
        schema = json.load(handle)
    types = copy.deepcopy(schema['types'])
    profile = copy.deepcopy(schema['profiles'][PROFILE])
    problems = apply(types, profile)
    overrides = profile['type_overrides']
    failures = list(problems)

    reach_cache = {}
    pool_hits = {}
    skipped_roots = 0
    for pool in profile['pools']:
        root = pool.get('root_type')
        if root is None or overrides.get(root, root) not in types:
            skipped_roots += 1
            continue
        if root not in reach_cache:
            reach_cache[root] = _reachable(types, overrides, root)
        for name in sorted(family):
            if name in reach_cache[root]:
                pool_hits.setdefault(name, []).append('%s(%s)' % (pool['name'], root))

    print('%-30s %5s %7s %6s  %s' % ('record', 'size', 'members', 'pools', 'status'))
    for record in RECORDS:
        name = record['name']
        applied = types[name]
        status = []
        if applied['size'] != record['size']:
            status.append('size %d != %d' % (applied['size'], record['size']))
        got = [(m['offset_bits'] // 8, m['name'], m['type']) for m in applied['members']]
        if got != record['members']:
            status.append('members differ from x64-zt')
        end = 0
        for m in applied['members']:
            if m['offset_bits'] < end:
                status.append('overlap at %s' % m['name'])
            end = max(end, m['offset_bits'] + m['size_bits'])
        if end > record['size'] * 8:
            status.append('extent %d > %d' % (end // 8, record['size']))
        failures.extend('%s: %s' % (name, s) for s in status)
        print('%-30s %5d %7d %6d  %s' % (name.replace('Replay::', ''), applied['size'], len(got),
                                       len(pool_hits.get(name, [])), 'OK' if not status else '; '.join(status)))

    print()
    print('Pool roots reaching each record (pool(root_type)); %d pools have no root type in the schema:'
          % skipped_roots)
    for record in RECORDS:
        hits = pool_hits.get(record['name'], [])
        print('  %s: %s' % (record['name'].replace('Replay::', ''), ', '.join(hits) if hits else 'none'))

    # Warnings only: Replay parents whose member widths disagree with the record they now embed.
    warnings = []
    for key in sorted(types):
        entry = types[key]
        if not key.startswith('Replay::') or entry.get('kind') not in ('struct', 'union'):
            continue
        for m in entry.get('members', []):
            target = overrides.get(_clean(m['type']), _clean(m['type']))
            if target not in family and target.split('::')[-1] not in bases:
                continue
            width = _size(types, overrides, m['type'])
            if width is not None and width * 8 != m['size_bits']:
                warnings.append('%s.%s: %s is %d bytes, member is %d' % (
                    key, m['name'] or '(base)', target, width, m['size_bits'] // 8))
    print()
    print('Embedding warnings (parents not yet re-laid out; not failures): %d' % len(warnings))
    for w in warnings:
        print('  WARN ' + w)

    # Cross-check against the x64-zt source when it is available.
    if os.path.exists(args.source):
        source, source_aliases = _read_source(args.source)
        for record in RECORDS:
            src = source.get(record['name'])
            if src is None:
                failures.append('%s: not in source' % record['name'])
            elif src['size'] != record['size'] or src['members'] != record['members']:
                failures.append('%s: differs from source file' % record['name'])
        source_bases = {b: t for b, t in source_aliases.items() if b in bases}
        if source_bases != ALIASES:
            failures.append('aliases differ from source: %r' % source_bases)
        print('Source compared: %s (%d family records, %d aliases)' % (args.source, len(RECORDS), len(source_bases)))
    else:
        print('Source not found, RECORDS not compared to it: %s' % args.source)

    print()
    if failures:
        for f in failures:
            print('FAIL ' + f)
        print('self-check: FAIL (%d problems)' % len(failures))
        return 1
    print('self-check: PASS (%d records: size, member offsets and order, extents; %d warnings)'
          % (len(RECORDS), len(warnings)))
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description='x64-zt GPU buffer record layouts for the Replay schema.')
    parser.add_argument('--check', action='store_true', help='verify apply() against RECORDS and the schema')
    parser.add_argument('--schema', default=SCHEMA, help='schema.json to read (read-only)')
    parser.add_argument('--source', default=SOURCE_LAYOUTS, help='x64-zt tools/iw8_layouts.txt')
    args = parser.parse_args(argv)
    if not args.check:
        parser.print_help()
        return 0
    return _check(args)


if __name__ == '__main__':
    raise SystemExit(main())
