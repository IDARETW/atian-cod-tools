"""Replay 1.20 world, model and terrain geometry records from x64-zt.

Source: E:/mw124/x64-zt tools/iw8_layouts.txt at x64-zt commit caf5b0f
(git -C E:/mw124/x64-zt log -1 --format=%h). Every record below copies its
members, sizes and evidence from that file; each evidence string is the comment
block above the record in the source and cites the Replay Load_* routine or RVA
that proves it. Offsets and sizes are bytes.

Covered: GfxWorldDrawVerts, GfxSubdivCache, XSurfaceSubdivInfo,
XSurfaceSubdivLevel, SurfaceBoundsExtra, VoxelRuntimeState, XModelRuntimeWord,
ReactiveMotionModelInfo, StMesh, StMeshLayerMaskData, StVertexData0, and the
three alias lines that name StVertexData0, StMeshLayerMaskData and StMesh.

Usage:
    python -I scripts/mw19/replay_layouts_x64zt_geometry.py --check

apply(types, profile) is meant for scripts/mw19/generate_schema.py to call after
replay_layouts.apply. It returns a list of problems; a record whose embedded member
types do not resolve is not written.

EXTERNAL_SIZES holds the x64-zt sizes of records owned by other families that these
records embed. --check compares the resolved schema sizes against them.
"""
RECORDS = [{'name': 'Replay::GfxWorldDrawVerts',
  'kind': 'struct',
  'size': 200,
  'members': [(0, 'posDataSize', 'unsigned int'),
              (4, 'auxDataSize', 'unsigned int'),
              (8, 'posData', 'unsigned __int8 *'),
              (16, 'auxData', 'unsigned __int8 *'),
              (24, 'posBuffer', 'Replay::GfxWrappedBuffer'),
              (88, 'auxBuffer', 'Replay::GfxWrappedBuffer'),
              (152, 'indexCount', 'unsigned int'),
              (160, 'indices', 'unsigned __int16 *'),
              (168, 'indexBuffer', 'Replay::GfxIndexBuffer')],
  'evidence': 'Load_GfxWorldDrawVerts loads 0xC8: the index buffer at +0xA8 is one 32-byte record.'},
 {'name': 'Replay::GfxSubdivCache',
  'kind': 'struct',
  'size': 104,
  'members': [(0, 'size', 'unsigned int'), (8, 'vertexBuffer', 'Replay::GfxWrappedRWBuffer')],
  'evidence': 'R_AllocSubdivCache (RVA 0x18C7D90) reads the size at +0 and creates the buffer at +8.'},
 {'name': 'Replay::XSurfaceSubdivInfo',
  'kind': 'struct',
  'size': 128,
  'members': [(0, 'levels', 'XSurfaceSubdivLevel *'),
              (8, 'flags', 'unsigned int'),
              (12, 'totalVertCount', 'unsigned int'),
              (16, 'totalRegularPatchCount', 'unsigned int'),
              (24, 'cache', 'Replay::GfxSubdivCache')],
  'evidence': 'q_Load_XSurfaceSubdivInfo loads 0x80; R_AllocSubdivCache binds the cache at +24.'},
 {'name': 'Replay::XSurfaceSubdivLevel',
  'kind': 'struct',
  'size': 568,
  'members': [(0, 'rigidVertLists', 'XSubdivRigidVertList *'),
              (8, 'faceCount', 'unsigned int'),
              (12, 'regularPatchCount', 'unsigned int'),
              (16, 'regularPatchOffset', 'unsigned int'),
              (20, 'facePointCount', 'unsigned int'),
              (24, 'facePointValence4Count', 'unsigned int'),
              (28, 'facePointBufferSize', 'unsigned int'),
              (32, 'edgePointCount', 'unsigned int'),
              (36, 'edgePointSmoothEnd', 'unsigned int'),
              (40, 'edgePointUVBorderEnd', 'unsigned int'),
              (44, 'vertexPointCount', 'unsigned int'),
              (48, 'vertexPointValence4Count', 'unsigned int'),
              (52, 'vertexPointBufferSize', 'unsigned int'),
              (56, 'normalCount', 'unsigned int'),
              (60, 'normalBufferSize', 'unsigned int'),
              (64, 'transitionPointCount', 'unsigned int'),
              (68, 'vertCount', 'unsigned int'),
              (72, 'vertOffset', 'unsigned int'),
              (80, 'faceIndices', 'unsigned __int16 *'),
              (88, 'regularPatchIndices', 'unsigned __int16 *'),
              (96, 'regularPatchFlags', 'unsigned int *'),
              (104, 'facePoints', 'unsigned int *'),
              (112, 'edgePoints', 'unsigned int *'),
              (120, 'vertexPoints', 'unsigned int *'),
              (128, 'normals', 'unsigned int *'),
              (136, 'transitionPoints', 'unsigned int *'),
              (144, 'regularPatchCones', 'float *'),
              (152, 'regularPatchIndexBuffer', 'Replay::GfxBufferResource'),
              (184, 'faceIndexBuffer', 'Replay::GfxBufferResource'),
              (216, 'regularPatchIndexBufferView', 'Replay::GfxShaderBufferView'),
              (232, 'regularPatchFlagsView', 'Replay::GfxShaderBufferView'),
              (248, 'facePointsView', 'Replay::GfxShaderBufferView'),
              (264, 'edgePointsView', 'Replay::GfxShaderBufferView'),
              (280, 'vertexPointsView', 'Replay::GfxShaderBufferView'),
              (296, 'normalsView', 'Replay::GfxShaderBufferView'),
              (312, 'transitionPointsView', 'Replay::GfxShaderBufferView'),
              (328, 'regularPatchConesView', 'Replay::GfxShaderBufferView'),
              (344, 'regularPatchFlagsBuffer', 'Replay::GfxBufferResource'),
              (376, 'regularPatchConesBuffer', 'Replay::GfxBufferResource'),
              (408, 'facePointsBuffer', 'Replay::GfxBufferResource'),
              (440, 'edgePointsBuffer', 'Replay::GfxBufferResource'),
              (472, 'vertexPointsBuffer', 'Replay::GfxBufferResource'),
              (504, 'normalsBuffer', 'Replay::GfxBufferResource'),
              (536, 'transitionPointsBuffer', 'Replay::GfxBufferResource')],
  'evidence': 'q_Load_XSurfaceSubdivLevelArray strides 0x238. q_R_AllocSurfaceSubdivLevels (RVA 0x18C8170) '
              'creates the index buffers at +0x98 and +0xB8, the views at +0xD8..+0x148 and the data buffers '
              'at +0x158..+0x228.'},
 {'name': 'Replay::SurfaceBoundsExtra',
  'kind': 'struct',
  'size': 32,
  'members': [(0, 'scalar', 'float'), (4, 'reserved', 'unsigned int[7]')],
  'evidence': 'GfxSurfaceBounds +0x18: Replay only (the game test build stops at the bounds). Stock zones '
              'hold a small positive float at +0 (for example 0x3707082B) and zeros after it. The meaning of '
              'the float is not verified.'},
 {'name': 'Replay::VoxelRuntimeState',
  'kind': 'struct',
  'size': 24,
  'members': [(0, 'voxelInternalNodeDynamicLightList', 'unsigned int *'),
              (8, 'voxelTreeHeaderCPU', 'GfxVoxelTreeHeader *'),
              (16, 'voxelTopDownViewNodeArrayCPU', 'GfxVoxelTopDownViewNode *')],
  'evidence': 'GfxVoxelTree +0x58: renderer pointers. Load_GfxVoxelTree and q_Postload_GfxVoxelTree (RVA '
              '0xDA2A50) stop at +0x58, and stock zones hold zeros. Names from the game test build, which '
              'has one more pointer here (inferred).'},
 {'name': 'Replay::XModelRuntimeWord',
  'kind': 'struct',
  'size': 8,
  'members': [(0, 'value', 'unsigned __int64')],
  'evidence': 'XModel +0x298: Replay only (the game test build has mdaoVolumes here). All 10516 models of 5 '
              'stock zones (mp_shipment, common_mp, global_mp, mp_donetsk, code_post_gfx) hold zero: the '
              'game fills it.'},
 {'name': 'Replay::ReactiveMotionModelInfo',
  'kind': 'struct',
  'size': 96,
  'members': [(0, 'windParams', 'ReactiveMotionWindParams'),
              (16, 'modelPartTweaks', 'ReactiveMotionModelPartTweaks *'),
              (24, 'modelParts', 'ReactiveMotionModelPart *'),
              (32, 'modelPartsBuffer', 'Replay::GfxWrappedBaseBuffer'),
              (80, 'numParts', 'unsigned __int8'),
              (81, 'flags', 'ReactiveMotionModelFlags'),
              (82, 'pad', 'unsigned __int8[2]'),
              (84, 'runtime_0x54', 'unsigned __int8[12]')],
  'evidence': 'Load_ReactiveMotionModelInfo loads 0x60 and reads the part count at +0x50. The loader does '
              'not touch +0x54..+0x5F.'},
 {'name': 'Replay::StMesh',
  'kind': 'struct',
  'size': 296,
  'members': [(0, 'vd0', 'Replay::StVertexData0'),
              (40, 'layerMask', 'Replay::StMeshLayerMaskData'),
              (112, 'indexBuffer', 'Replay::GfxIndexBuffer'),
              (144, 'indices', 'unsigned __int16 *'),
              (152, 'streamKey', 'StreamKey *'),
              (160, 'vertexBufferToRelease', 'Replay::GfxVertexBuffer'),
              (192, 'indexBufferToRelease', 'Replay::GfxIndexBuffer'),
              (224, 'layerMaskBufferToRelease', 'Replay::GfxWrappedBuffer'),
              (288, 'vertexCount', 'unsigned __int16'),
              (290, 'triCount', 'unsigned __int16'),
              (292, 'cutoutTriCount', 'unsigned __int16'),
              (294, 'flags', 'StMeshFlags')],
  'evidence': 'Load_StVertexData0 loads 0x28, Load_StMeshLayerMaskData 0x48, Load_StMesh 0x128. Load_StMesh '
              'reads the triangle count at +0x122; Load_StVertexData0 reads the vertex count at +0x120. The '
              'loaders do not touch +0xA0..+0x11F; the release copies there are inferred from the game-test '
              'member order and the Replay record sizes.'},
 {'name': 'Replay::StMeshLayerMaskData',
  'kind': 'struct',
  'size': 72,
  'members': [(0, 'data', 'unsigned __int8 *'), (8, 'buffer', 'Replay::GfxWrappedBuffer')],
  'evidence': 'Load_StVertexData0 loads 0x28, Load_StMeshLayerMaskData 0x48, Load_StMesh 0x128. Load_StMesh '
              'reads the triangle count at +0x122; Load_StVertexData0 reads the vertex count at +0x120. The '
              'loaders do not touch +0xA0..+0x11F; the release copies there are inferred from the game-test '
              'member order and the Replay record sizes.'},
 {'name': 'Replay::StVertexData0',
  'kind': 'struct',
  'size': 40,
  'members': [(0, 'data', 'unsigned __int8 *'), (8, 'vb', 'Replay::GfxVertexBuffer')],
  'evidence': 'Load_StVertexData0 loads 0x28, Load_StMeshLayerMaskData 0x48, Load_StMesh 0x128. Load_StMesh '
              'reads the triangle count at +0x122; Load_StVertexData0 reads the vertex count at +0x120. The '
              'loaders do not touch +0xA0..+0x11F; the release copies there are inferred from the game-test '
              'member order and the Replay record sizes.'}]

ALIASES = {'StVertexData0': 'Replay::StVertexData0',
 'StMeshLayerMaskData': 'Replay::StMeshLayerMaskData',
 'StMesh': 'Replay::StMesh'}

EXTERNAL_SIZES = {'Replay::GfxWrappedBuffer': 64,
 'Replay::GfxIndexBuffer': 32,
 'Replay::GfxWrappedRWBuffer': 96,
 'Replay::GfxBufferResource': 32,
 'Replay::GfxShaderBufferView': 16,
 'Replay::GfxWrappedBaseBuffer': 48,
 'Replay::GfxVertexBuffer': 32}


import json
import os
import re
import sys
from copy import deepcopy

SCALAR_SIZES = {
    'bool': 1, 'char': 1, 'signed char': 1, 'unsigned char': 1, '__int8': 1, 'unsigned __int8': 1,
    'short': 2, 'unsigned short': 2, '__int16': 2, 'unsigned __int16': 2,
    'int': 4, 'unsigned int': 4, 'long': 4, 'unsigned long': 4, 'float': 4,
    '__int32': 4, 'unsigned __int32': 4,
    'double': 8, '__int64': 8, 'unsigned __int64': 8, 'long long': 8, 'unsigned long long': 8,
}
FAMILY = {record['name'] for record in RECORDS}
# Base names owned by this family. Their replacements are written from the x64-zt
# records, so propagation must never clone a base record of these names.
FAMILY_BASE = {name[len('Replay::'):] for name in FAMILY} | set(ALIASES)
PROFILE = 'replay-1.20'
# Replay record that replay_layouts-style propagation is expected to re-lay out.
PROPAGATED_EXPECTED = {'StTerrainNode'}


class _Missing(Exception):
    pass


def _size(types, overrides, name, written=None):
    """Byte size of a member type. Pointers are 8 bytes; arrays multiply their element."""
    name = name.strip()
    if name.endswith('*'):
        return 8
    match = re.match(r'^(.+)\[(\d+)\]$', name)
    if match:
        return _size(types, overrides, match.group(1), written) * int(match.group(2))
    if name in SCALAR_SIZES:
        return SCALAR_SIZES[name]
    if written is not None and name in FAMILY and name not in written:
        raise _Missing(name)
    key = overrides.get(name, name)
    if key not in types:
        raise _Missing(name)
    return types[key]['size']


def _missing_names(types, overrides, written, record):
    names = set()
    for _, _, type_name in record['members']:
        try:
            _size(types, overrides, type_name, written)
        except _Missing as missing:
            names.add(missing.args[0])
    return sorted(names)


def _overlap_problem(record, members):
    end = 0
    for member in members:
        if member['offset_bits'] < end:
            return '%s: member %s overlaps the previous member' % (record['name'], member['name'])
        end = member['offset_bits'] + member['size_bits']
    if end > record['size'] * 8:
        return '%s: members end at %d, past the record size %d' % (record['name'], end // 8, record['size'])
    return None


def _clone(types, overrides, name, size):
    target = 'Replay::' + name
    value = deepcopy(types[name])
    value.update(name=target, size=size)
    types[target] = value
    overrides[name] = target
    return value


def _propagate(types, overrides):
    """Re-lay out containers of the replaced records, using the rule in replay_layouts.py."""
    for _ in range(32):
        changed = False
        for name in list(types):
            if name.startswith(('Replay', 'ReplayFDA')) or name.endswith('*') or name in FAMILY_BASE:
                continue
            current = types[overrides.get(name, name)]
            if current['kind'] == 'array':
                size = _size(types, overrides, current['element']) * current['count']
                if size != current['size']:
                    if name not in overrides:
                        current = _clone(types, overrides, name, current['size'])
                    current['size'] = size
                    changed = True
                continue
            if current['kind'] != 'struct':
                continue
            updated, shift = [], 0
            for original in current['members']:
                member = deepcopy(original)
                target = member['type']
                delta = 0
                if not target.endswith('*') and (target in overrides or target.startswith('Replay::')):
                    native = types[overrides.get(target, target)]
                    if native['kind'] in ('struct', 'array', 'opaque', 'union'):
                        delta = native['size'] * 8 - member['size_bits']
                member['offset_bits'] += shift
                member['size_bits'] += delta
                shift += delta
                updated.append(member)
            if shift:
                if name not in overrides:
                    current = _clone(types, overrides, name, current['size'])
                current.update(size=current['size'] + shift // 8, members=updated)
                changed = True
        if not changed:
            return
    raise RuntimeError('Replay geometry propagation did not converge')


def apply(types, profile):
    """Write the x64-zt geometry records into types and set the alias overrides.

    Returns a list of problems. A record is written only when every embedded member
    type resolves; unresolved records are reported and not written, and their aliases
    are left unset."""
    overrides = profile.setdefault('type_overrides', {})
    problems = []
    written = set()
    pending = list(RECORDS)
    progress = True
    while progress and pending:
        progress = False
        for record in list(pending):
            try:
                members = []
                for offset, name, type_name in record['members']:
                    size = _size(types, overrides, type_name, written)
                    members.append(dict(name=name, offset_bits=offset * 8, size_bits=size * 8, type=type_name))
            except _Missing:
                continue
            members.sort(key=lambda m: m['offset_bits'])
            pending.remove(record)
            progress = True
            problem = _overlap_problem(record, members)
            if problem:
                problems.append(problem)
                continue
            types[record['name']] = dict(name=record['name'], kind=record['kind'],
                                         size=record['size'], members=members)
            written.add(record['name'])
    for record in pending:
        problems.append('%s not written: unresolved member types %s' % (
            record['name'], ', '.join(_missing_names(types, overrides, written, record))))
    for base, target in ALIASES.items():
        if target in written:
            overrides[base] = target
        else:
            problems.append('alias %s -> %s not set: record not written' % (base, target))
    _propagate(types, overrides)
    return problems


def _self_check():
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    path = os.path.join(root, 'config', 'data', 'mw19', 'schema.json')
    with open(path, encoding='utf-8') as handle:
        schema = json.load(handle)
    before_types = schema['types']
    types = deepcopy(before_types)
    profile = deepcopy(schema['profiles'][PROFILE])
    before_overrides = deepcopy(profile.get('type_overrides', {}))
    problems = apply(types, profile)
    overrides = profile['type_overrides']
    failures = []
    failures.extend('APPLY ' + problem for problem in problems)

    # Per-record checks against x64-zt: size, member order, offsets, names, types, overlap, dependency sizes.
    verified = 0
    print('%-34s %6s %6s  %-9s %s' % ('RECORD', 'X64ZT', 'SCHEMA', 'STATUS', 'DETAIL'))
    for record in RECORDS:
        name = record['name']
        entry = types.get(name)
        record_problems = [p for p in problems if p.split(' ')[0].rstrip(':') == name]
        if entry is None or record_problems:
            failures.append('%s not written to types' % name)
            embedders = sorted(k for k, e in before_types.items() if k not in FAMILY and 'members' in e
                               and any(m['type'].strip() == name[len('Replay::'):] for m in e['members']))
            print('%-34s %6d %6s  %-9s %s' % (name, record['size'], '-', 'NOTWRITTEN', '; '.join(record_problems)))
            if embedders:
                print('    base-name embedders still at schema size: %s' % ', '.join(embedders))
            continue
        before_failures = len(failures)
        expected = [(offset, mname, mtype) for offset, mname, mtype in record['members']]
        actual = [(m['offset_bits'] // 8, m['name'], m['type']) for m in entry.get('members', [])]
        if entry['kind'] != record['kind']:
            failures.append('%s kind %s != x64-zt %s' % (name, entry['kind'], record['kind']))
        if entry['size'] != record['size']:
            failures.append('%s size %d != x64-zt %d' % (name, entry['size'], record['size']))
        if actual != expected:
            failures.append('%s members differ from x64-zt' % name)
        end = 0
        for member in entry.get('members', []):
            if member['offset_bits'] < end:
                failures.append('%s member %s overlaps the previous member' % (name, member['name']))
            end = member['offset_bits'] + member['size_bits']
        if end > entry['size'] * 8:
            failures.append('%s members exceed the record size' % name)
        dependency = []
        for member in entry.get('members', []):
            base = re.sub(r'\[\d+\]$', '', member['type'].strip())
            if not base.endswith('*') and base in EXTERNAL_SIZES:
                resolved = _size(types, overrides, base)
                if resolved != EXTERNAL_SIZES[base]:
                    dependency.append('%s is %d, x64-zt %d' % (base, resolved, EXTERNAL_SIZES[base]))
        local = len(failures) > before_failures
        if not local and not dependency:
            verified += 1
        status = 'FAIL' if local else ('DEPFAIL' if dependency else 'OK')
        detail = '; '.join(dependency)
        failures.extend('%s embeds %s (owned by another family)' % (name, d) for d in dependency)
        print('%-34s %6d %6d  %-9s %s' % (name, record['size'], entry['size'], status, detail))

    for base, target in sorted(ALIASES.items()):
        ok = overrides.get(base) == target
        if not ok:
            failures.append('alias %s -> %s not in type_overrides' % (base, target))
        print('ALIAS %-28s -> %-30s %s' % (base, target, 'OK' if ok else 'UNSET'))

    for name in sorted(EXTERNAL_SIZES):
        if name not in types:
            print('DEPENDENCY %-30s not in types (x64-zt %d)' % (name, EXTERNAL_SIZES[name]))

    # Pool reachability through struct members, array elements and pointer targets.
    def children(node):
        entry = types.get(node)
        if entry is None:
            return []
        kind = entry.get('kind')
        if kind in ('struct', 'union'):
            return [m['type'] for m in entry.get('members', [])]
        if kind == 'array':
            return [entry['element']]
        if kind == 'pointer':
            return [entry['target']]
        return []

    def reach(root_name):
        seen = set()
        stack = [overrides.get(root_name, root_name)]
        while stack:
            node = stack.pop()
            if node in seen or node not in types:
                continue
            seen.add(node)
            for child in children(node):
                child = re.sub(r'\[\d+\]$', '', child.strip()).rstrip('*').strip()
                stack.append(overrides.get(child, child))
        return seen

    watch = sorted(FAMILY | {ALIASES[a] for a in ALIASES})
    watch.append('Replay::StTerrainNode')
    reached = {}
    for pool in profile['pools']:
        root = pool.get('root_type')
        if not root:
            continue
        for node in reach(root):
            if node in watch:
                reached.setdefault(node, []).append('%s(%s)' % (pool['name'], root))
    print()
    print('POOL REACHABILITY (pool roots whose type graph reaches each overridden type)')
    for node in watch:
        pools = sorted(reached.get(node, []))
        print('%-34s %s' % (node, ', '.join(pools) if pools else 'none'))

    # Everything else that changed must be a propagated container of a changed record.
    changed = {k for k in set(types) | set(before_types) if types.get(k) != before_types.get(k)}
    propagated = sorted(k for k in changed if k not in FAMILY and k == 'Replay::StTerrainNode')
    unexpected = sorted(k for k in changed if k not in FAMILY and k not in propagated)
    unexpected += sorted('type_overrides[%s]' % k for k in set(overrides) | set(before_overrides)
                         if overrides.get(k) != before_overrides.get(k)
                         and k not in ALIASES and k not in PROPAGATED_EXPECTED)
    failures.extend('unexpected change: %s' % k for k in unexpected)
    print()
    print('PROPAGATED CONTAINERS: %s' % (', '.join('%s %d -> %d bytes' % (
        k, before_types[k.replace('Replay::', '')]['size'], types[k]['size']) for k in propagated) or 'none'))
    print('UNEXPECTED CHANGES: %d %s' % (len(unexpected), ', '.join(unexpected)))
    for problem in problems:
        print('PROBLEM ' + problem)
    print()
    dependency_count = sum(1 for f in failures if 'owned by another family' in f)
    print('SELF-CHECK %s: %d/%d records match x64-zt, %d failures (%d dependency size mismatches), '
          '%d apply problems, %d unexpected changes, %d propagated containers'
          % ('PASS' if not failures else 'FAIL', verified, len(RECORDS), len(failures), dependency_count,
             len(problems), len(unexpected), len(propagated)))
    return 0 if not failures else 1


if __name__ == '__main__':
    if '--check' in sys.argv[1:]:
        sys.exit(_self_check())
    print('usage: python -I scripts/mw19/replay_layouts_x64zt_geometry.py --check')
    sys.exit(2)
