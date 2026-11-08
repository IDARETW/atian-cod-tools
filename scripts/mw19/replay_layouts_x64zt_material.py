"""Replay 1.20 material, technique and constant-buffer records, ported from x64-zt.

Source: x64-zt tools/iw8_layouts.txt at git commit caf5b0f (E:/mw124/x64-zt). The records are
Replay::MaterialPipelineState, Replay::MaterialTechniqueStateBits, Replay::MaterialTechnique and
Replay::MaterialConstantBufferDef, plus the alias lines that name the last three. Offsets and sizes
are bytes. Member lists are (offset, name, type) exactly as the source file writes them.

apply(types, profile) writes the Replay records into a schema type table and sets the profile's
type_overrides. It returns a list of problems. It never creates a type it cannot size from the
schema, so unresolved member types are reported, not invented.

Self-check: python -I scripts/mw19/replay_layouts_x64zt_material.py --check [--x64zt PATH]
"""
import importlib.util
import json
import os
import re
import sys
from copy import deepcopy
from pathlib import Path

X64ZT_COMMIT = 'caf5b0f'
X64ZT_LAYOUTS = 'E:/mw124/x64-zt/tools/iw8_layouts.txt'
ROOT = Path(__file__).resolve().parents[2]
SCHEMA = ROOT / 'config' / 'data' / 'mw19' / 'schema.json'

# Comment block above the group that holds MaterialPipelineState, MaterialTechniqueStateBits and
# MaterialTechnique (x64-zt lines 198-204). The three records have no comment of their own.
MTL_EVIDENCE = (
    'Load_MaterialTechnique (RVA 0xE0CED0) loads 0xB8. Serialized members: the name, the pipeline '
    'state records at +0x28 (count = +0x83 * +0x0E, 16 bytes each), a 16-byte map at +0x50, the four '
    'stage shaders at +0x58..+0x70 (vertex, hull, domain, pixel shader pools), the state bits at +0x88 '
    '(+0x83 records of 5 bytes) and the shader arguments at +0xB0 (sum of the four counts at '
    '+0x78..+0x7B). The 16-byte record at +0xA0 is loaded in place. Stock headers hold the stable '
    'argument size at +0x80 and the layout index at +0x9C. Names of members the loader does not use '
    'follow the game-test order (inferred).')

# Comment block above the GfxConstantBuffer group (x64-zt lines 349-351). MaterialConstantBufferDef
# (line 357) has no comment of its own.
MCBD_EVIDENCE = (
    'q_Load_MaterialConstantBufferDef (RVA 0xE0C5F0) loads 0x110; q_Load_MaterialConstantBufferTable '
    '(RVA 0x194B9D0) creates the four stage buffers at +0x30, +0x68, +0xA0 and +0xD8 (0x38 apart). '
    'The members after the game-test descriptor are runtime only (inferred).')

RECORDS = [
    dict(name='Replay::MaterialPipelineState', kind='struct', size=16, evidence=MTL_EVIDENCE,
         members=[
             (0, 'hash', 'unsigned __int64'),
             (8, 'pso', 'ID3D12PipelineState *'),
         ]),
    dict(name='Replay::MaterialTechniqueStateBits', kind='struct', size=5, evidence=MTL_EVIDENCE,
         members=[
             (0, 'bits', 'unsigned __int8[5]'),
         ]),
    dict(name='Replay::MaterialTechnique', kind='struct', size=184, evidence=MTL_EVIDENCE,
         members=[
             (0, 'name', 'const char *'),
             (8, 'type', 'MaterialTechniqueType'),
             (12, 'flags', 'unsigned __int16'),
             (14, 'pipelineStateInputLayoutCount', 'unsigned __int8'),
             (15, 'pipelineStateMap', 'unsigned __int8[25]'),
             (40, 'pipelineStates', 'Replay::MaterialPipelineState *'),
             (48, 'customBufferFlags', 'unsigned __int64[4]'),
             (80, 'rootParameterMap', 'unsigned __int8 *'),
             (88, 'vertexShader', 'ComputeShader *'),
             (96, 'hullShader', 'ComputeShader *'),
             (104, 'domainShader', 'ComputeShader *'),
             (112, 'pixelShader', 'ComputeShader *'),
             (120, 'perPrimArgCount', 'unsigned __int8'),
             (121, 'perObjArgCount', 'unsigned __int8'),
             (122, 'stableArgCount', 'unsigned __int8'),
             (123, 'neverArgCount', 'unsigned __int8'),
             (124, 'perPrimArgSize', 'unsigned __int16'),
             (126, 'perObjArgSize', 'unsigned __int16'),
             (128, 'stableArgSize', 'unsigned __int16'),
             (130, 'colorRtCount', 'unsigned __int8'),
             (131, 'pipelineStateCount', 'unsigned __int8'),
             (136, 'stateBits', 'Replay::MaterialTechniqueStateBits *'),
             (144, 'renderTargetFormats', 'MaterialRenderTargetFormat'),
             (152, 'customSamplerFlags', 'unsigned int'),
             (156, 'precompiledIndex', 'MaterialConstantLayout'),
             (157, 'stageConfig', 'MaterialStageConfig'),
             (160, 'defaultStateBits', 'GfxStateBits'),
             (176, 'args', 'MaterialShaderArgument *'),
         ]),
    dict(name='Replay::MaterialConstantBufferDef', kind='struct', size=272, evidence=MCBD_EVIDENCE,
         members=[
             (0, 'vsDataSize', 'unsigned int'),
             (4, 'hsDataSize', 'unsigned int'),
             (8, 'dsDataSize', 'unsigned int'),
             (12, 'psDataSize', 'unsigned int'),
             (16, 'vsData', 'unsigned __int8 *'),
             (24, 'hsData', 'unsigned __int8 *'),
             (32, 'dsData', 'unsigned __int8 *'),
             (40, 'psData', 'unsigned __int8 *'),
             (48, 'vsConstantBuffer', 'Replay::GfxConstantBuffer'),
             (104, 'hsConstantBuffer', 'Replay::GfxConstantBuffer'),
             (160, 'dsConstantBuffer', 'Replay::GfxConstantBuffer'),
             (216, 'psConstantBuffer', 'Replay::GfxConstantBuffer'),
         ]),
]

# Alias lines of x64-zt that name a record of this family.
ALIASES = {
    'MaterialTechnique': 'Replay::MaterialTechnique',
    'MaterialPipelineState': 'Replay::MaterialPipelineState',
    'MaterialConstantBufferDef': 'Replay::MaterialConstantBufferDef',
}

# Replay records that these members use and that no schema type provides yet (owned by another
# family). The size is the x64-zt record size. Used only to size the member; apply() reports it.
EXTERNAL_SIZES = {
    'Replay::GfxConstantBuffer': 56,
}

SCALARS = {
    'char': 1, 'signed char': 1, 'unsigned char': 1, '__int8': 1, 'unsigned __int8': 1, 'bool': 1,
    'short': 2, 'unsigned short': 2, '__int16': 2, 'unsigned __int16': 2, 'wchar_t': 2,
    'int': 4, 'unsigned int': 4, 'long': 4, 'unsigned long': 4, '__int32': 4, 'unsigned __int32': 4,
    'float': 4, 'double': 8, '__int64': 8, 'unsigned __int64': 8, 'long long': 8,
    'unsigned long long': 8,
}


def _strip_array(name):
    match = re.match(r'^(.*)\[(\d+)\]$', name)
    if match:
        return match.group(1).strip(), int(match.group(2))
    return name, None


def _base(name):
    """Base type name with array and pointer suffixes removed."""
    name = name.strip()
    while True:
        inner, _ = _strip_array(name)
        if inner == name:
            break
        name = inner
    while name.endswith('*'):
        name = name[:-1].strip()
    return name


def _size(types, name):
    """Size in bytes of a member type, or None when no schema type or external size gives it."""
    name = name.strip()
    if name.endswith('*'):
        return 8
    inner, count = _strip_array(name)
    if count is not None:
        element = _size(types, inner)
        return None if element is None else element * count
    if name in SCALARS:
        return SCALARS[name]
    if name in types:
        return types[name]['size']
    return EXTERNAL_SIZES.get(name)


def _missing(types, name):
    """The schema type name that a member spelling needs but the schema lacks, else None."""
    name = name.strip()
    if name.endswith('*'):
        return _missing(types, name[:-1])
    inner, count = _strip_array(name)
    if count is not None:
        return _missing(types, inner)
    if name.startswith('const '):
        name = name[len('const '):].strip()
    if name in SCALARS or name in types:
        return None
    return name


def apply(types, profile):
    """Write the Replay material records into types, set the aliases, return the problems list."""
    problems = []
    overrides = profile.setdefault('type_overrides', {})

    for rec in RECORDS:
        types[rec['name']] = dict(name=rec['name'], size=rec['size'], kind=rec['kind'], members=[])

    # Pointer spellings used by the members, once their targets exist.
    for rec in RECORDS:
        for _, _, type_name in rec['members']:
            if type_name.endswith('*') and type_name not in types:
                target = type_name[:-1].strip()
                if target in types:
                    types[type_name] = dict(name=type_name, kind='pointer', size=8, target=target)

    for rec in RECORDS:
        members = []
        for offset, name, type_name in rec['members']:
            size = _size(types, type_name)
            if size is None:
                problems.append('%s.%s: type %s has no size in the schema; member omitted'
                                % (rec['name'], name, type_name))
                continue
            missing = _missing(types, type_name)
            if missing is not None:
                problems.append('%s.%s: type %s is not in the schema; no type entry created '
                                '(size %d from x64-zt)' % (rec['name'], name, missing, size))
            members.append(dict(name=name, offset_bits=offset * 8, size_bits=size * 8, type=type_name))
        types[rec['name']]['members'] = members

    for base, replay in ALIASES.items():
        overrides[base] = replay
    return problems


def _canon(name, overrides):
    base = _base(name)
    return overrides.get(base, base)


def _reach(types, overrides, root):
    """Every canonical type name reachable from root through struct members, pointers and arrays."""
    seen = set()
    stack = [root]
    while stack:
        raw = stack.pop()
        if not raw:
            continue
        name = _canon(raw, overrides)
        if name in seen:
            continue
        seen.add(name)
        entry = types.get(name)
        if entry is None:
            continue
        kind = entry.get('kind')
        if kind in ('struct', 'union'):
            stack.extend(m['type'] for m in entry.get('members', []))
        elif kind == 'pointer':
            stack.append(entry.get('target'))
        elif kind == 'array':
            stack.append(entry.get('element'))
    return seen


def _evidence_from_source(lines, name):
    """The comment block above the nearest 'struct|union <name>' line, comment marks removed."""
    index = next((i for i, line in enumerate(lines)
                  if re.match(r'^(struct|union) %s \d+\s*$' % re.escape(name), line)), None)
    if index is None:
        return None
    j = index - 1
    while j >= 0 and not lines[j].startswith('#'):
        j -= 1
    block = []
    while j >= 0 and lines[j].startswith('#'):
        block.insert(0, lines[j][1:].strip())
        j -= 1
    return ' '.join(block)


def _load_x64zt_parser(path):
    spec = importlib.util.spec_from_file_location('x64zt_iw8_layouts', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _check(argv):
    layouts = X64ZT_LAYOUTS
    if '--x64zt' in argv:
        layouts = argv[argv.index('--x64zt') + 1]

    with open(SCHEMA, encoding='utf-8') as handle:
        schema = json.load(handle)
    types = deepcopy(schema['types'])
    profile = deepcopy(schema['profiles']['replay-1.20'])
    problems = apply(types, profile)
    overrides = profile['type_overrides']
    mismatches = []

    for rec in RECORDS:
        name = rec['name']
        entry = types.get(name)
        if entry is None:
            mismatches.append('%s: not written' % name)
            continue
        if entry['size'] != rec['size']:
            mismatches.append('%s: size %d, x64-zt %d' % (name, entry['size'], rec['size']))
        got = [(m['offset_bits'] // 8, m['name'], m['type']) for m in entry['members']]
        if got != [tuple(m) for m in rec['members']]:
            mismatches.append('%s: members %r differ from x64-zt %r' % (name, got, rec['members']))
        end = 0
        for m in entry['members']:
            start, width = m['offset_bits'], m['size_bits']
            if width != 8 * (_size(types, m['type']) or 0):
                mismatches.append('%s.%s: size_bits %d does not match type %s'
                                  % (name, m['name'], width, m['type']))
            if start < end:
                mismatches.append('%s.%s: overlaps the previous member' % (name, m['name']))
            end = max(end, start + width)
        if end > rec['size'] * 8:
            mismatches.append('%s: members end at %d, past size %d' % (name, end // 8, rec['size']))

    drift = 'skipped (x64-zt file not found: %s)' % layouts
    if os.path.exists(layouts):
        parser = _load_x64zt_parser(os.path.join(os.path.dirname(layouts), 'iw8_layouts.py'))
        source, source_aliases = parser.parse(layouts)
        with open(layouts, encoding='utf-8') as handle:
            lines = handle.read().splitlines()
        for rec in RECORDS:
            src = source.get(rec['name'])
            if src is None or src['kind'] != rec['kind'] or src['size'] != rec['size'] \
                    or [tuple(m) for m in src['members']] != [tuple(m) for m in rec['members']]:
                mismatches.append('%s: differs from %s' % (rec['name'], layouts))
            if _evidence_from_source(lines, rec['name']) != rec['evidence']:
                mismatches.append('%s: evidence differs from %s' % (rec['name'], layouts))
        for base, replay in ALIASES.items():
            if source_aliases.get(base) != replay:
                mismatches.append('alias %s: x64-zt has %r' % (base, source_aliases.get(base)))
        drift = 'matches %s (commit %s expected)' % (layouts, X64ZT_COMMIT)

    rows = []
    pools = [p for p in profile.get('pools', []) if p.get('root_type')]
    for rec in RECORDS:
        target = rec['name']
        reached = [p['name'] + '(' + p['root_type'] + ')' for p in pools
                   if target in _reach(types, overrides, p['root_type'])]
        entry = types.get(target, {})
        rows.append((target, rec['size'], entry.get('size', '-'), len(rec['members']),
                     ', '.join(reached) if reached else 'none'))

    print('%-38s %8s %8s %4s  %s' % ('record', 'x64-zt', 'applied', 'mem', 'pool roots reaching it'))
    for row in rows:
        print('%-38s %8s %8s %4s  %s' % row)
    print('aliases: ' + ', '.join('%s -> %s' % (b, r) for b, r in ALIASES.items()))
    print('x64-zt drift check: ' + drift)
    for problem in problems:
        print('PROBLEM: ' + problem)
    for mismatch in mismatches:
        print('MISMATCH: ' + mismatch)
    status = 'FAIL' if mismatches else 'PASS'
    print('SELF-CHECK %s: %d records, %d mismatches, %d problems (unresolved member types, '
          'owned by other families); commit %s'
          % (status, len(RECORDS), len(mismatches), len(problems), X64ZT_COMMIT))
    return 1 if mismatches else 0


if __name__ == '__main__':
    if '--check' in sys.argv[1:]:
        sys.exit(_check(sys.argv[1:]))
    print('usage: python -I scripts/mw19/replay_layouts_x64zt_material.py --check [--x64zt PATH]')
    sys.exit(2)
