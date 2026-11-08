"""Replay 1.20 transient streaming, animation, weapon and sound records (x64-zt port).

Source: x64-zt tools/iw8_layouts.txt at commit caf5b0f (git -C E:/mw124/x64-zt log -1 --format=%h).
Every offset and size in this file is in bytes. RECORDS holds each record's name, kind, size and
members (offset, name, type) exactly as the source spells them; each evidence string is the comment
block directly above the record in the source (Replay Load_* names and RVAs).

Records covered: TransientCosts, TransientInfo, MayhemAnimFramesUncompressed, PlayerAnimEntry,
PlayerAnimsetAlias, PlayerConditionMask, AttProjectile, SndMusicAsset, SndMusicState.

apply(types, profile) writes types['Replay::<Name>'] and sets profile['type_overrides'][base].
It returns a list of problems (member types that the schema does not define) instead of inventing them.
Call it after replay_layouts.apply(): that function writes Replay::PlayerConditionMask as an opaque
48-byte entry, and this module replaces it with the x64-zt struct. In the other order the opaque entry wins.

Self-check (read-only on config/data/mw19/schema.json):
    python -I scripts/mw19/replay_layouts_x64zt_misc.py --check
"""
import json
import os
import re
import sys
from copy import deepcopy

SOURCE = 'x64-zt tools/iw8_layouts.txt'
SOURCE_COMMIT = 'caf5b0f'

RECORDS = [
    dict(
        name='Replay::TransientCosts',
        kind='struct',
        size=48,
        evidence='Load_TransientCosts (RVA 0xE30B70) loads 0x30: one content set at +0, the cost array at +0x10 (+0x28 records of 4 bytes), page sizes at +0x18 (+0x2C entries) and +0x20 (+0x28 entries).',
        members=[
            (0, 'streamContents', 'StreamingContentSet'),
            (16, 'streamingCosts', 'StreamingItemCost *'),
            (24, 'residentPageSizes', 'unsigned __int16 *'),
            (32, 'tempPageSizes', 'unsigned __int16 *'),
            (40, 'streamingItemCount', 'unsigned int'),
            (44, 'transientCostCount', 'unsigned int'),
        ]),
    dict(
        name='Replay::TransientInfo',
        kind='struct',
        size=184,
        evidence='q_Load_TransientInfo (RVA 0xE30CA0) loads 0xB8. The reference arrays at +0x48/+0x50/+0x58 take their counts from +0xA0/+0xA4/+0xA8, the tile maps at +0x60 from +0xAC, the grid bounds at +0x70 from +0x80, the lookup at +0x78 from +0x88; the skip-loading bits sit at +0x90. The loader does not touch +0x84, +0x8C, +0xB0 and +0xB4 (names from the game-test order, inferred).',
        members=[
            (0, 'transientSets', 'TransientSets'),
            (24, 'transientCosts', 'Replay::TransientCosts'),
            (72, 'forcedStubAssets', 'ForcedStubAssetReference *'),
            (80, 'transientFiles', 'TransientFileReference *'),
            (88, 'transientAssets', 'TransientAssetReference *'),
            (96, 'transientWorldGridTileMaps', 'TransientWorldGridTileMap *'),
            (104, 'transientWorldGridSpatial', 'SpatialPartition_Tree *'),
            (112, 'transientWorldGridBounds', 'Bounds *'),
            (120, 'transientWorldGridHighLodLookup', 'unsigned __int16 *'),
            (128, 'transientWorldGridBoundsCount', 'unsigned int'),
            (132, 'transientWorldGridHighLodCount', 'unsigned int'),
            (136, 'transientWorldGridHighLodLookupCount', 'unsigned int'),
            (140, 'transientInfoFlags', 'TransientInfoFlags'),
            (144, 'transientSkipLoading', 'bitarray_dynamic'),
            (160, 'forcedStubAssetCount', 'unsigned int'),
            (164, 'transientFileCount', 'unsigned int'),
            (168, 'transientAssetCount', 'unsigned int'),
            (172, 'transientWorldGridTileMapCount', 'unsigned int'),
            (176, 'transientCollisionTileCount', 'unsigned int'),
            (180, 'transientMPCommonCount', 'unsigned int'),
        ]),
    dict(
        name='Replay::MayhemAnimFramesUncompressed',
        kind='struct',
        size=128,
        evidence='Load_MayhemAnimFramesUncompressed loads 0x80: the buffers at +0x18 and +0x48, the strides at +0x78 and +0x7C.',
        members=[
            (0, 'diskQuat', 'unsigned __int8 *'),
            (8, 'diskPos', 'unsigned __int8 *'),
            (16, 'transBounds', 'MayhemTransBounds *'),
            (24, 'quat', 'Replay::GfxWrappedBaseBuffer'),
            (72, 'pos', 'Replay::GfxWrappedBaseBuffer'),
            (120, 'quatStride', 'unsigned int'),
            (124, 'posStride', 'unsigned int'),
        ]),
    dict(
        name='Replay::PlayerAnimEntry',
        kind='struct',
        size=88,
        evidence='The player animation script loader (RVA 0xDDEAB0) strides the entries 0x58 and maps the script strings at +0, +4 and +8. The game test build has eight move type words in 104 bytes; Replay has 16 bytes less, so four words, as in PlayerAnimsetAlias (inferred).',
        members=[
            (0, 'entryName', 'scr_string_t'),
            (4, 'animName', 'scr_string_t'),
            (8, 'shadowAnimName', 'scr_string_t'),
            (16, 'flags', 'unsigned __int64'),
            (24, 'animIndex', 'unsigned __int64'),
            (32, 'shadowAnimIndex', 'unsigned __int64'),
            (40, 'movetype', 'unsigned int[4]'),
            (56, 'moveSpeed', 'float'),
            (60, 'moveSpeedAdjust', 'float'),
            (64, 'initialLerp', '__int16'),
            (66, 'additiveAdjustBlendout', '__int16'),
            (68, 'duration', 'unsigned __int16'),
            (70, 'fastDuration', 'unsigned __int16'),
            (72, 'additiveAdjustAnim', 'unsigned __int16'),
            (74, 'slowAnimIndex', 'unsigned __int16'),
            (76, 'bodyPart', 'unsigned __int8'),
            (77, 'serverNoteType', 'unsigned __int8'),
            (78, 'clientNoteType', 'unsigned __int8'),
            (79, 'aimSet', 'unsigned __int8'),
            (80, 'mantleSet', 'unsigned __int8'),
            (81, 'leanSet', 'unsigned __int8'),
            (82, 'turns', 'unsigned __int8'),
            (83, 'twitches', 'unsigned __int8'),
            (84, 'syncGroup', 'unsigned __int8'),
        ]),
    dict(
        name='Replay::PlayerAnimsetAlias',
        kind='struct',
        size=136,
        evidence='Load_PlayerAnimsetAlias (RVA 0xDD1A40) loads 0x88 and maps the script strings at +0x20..+0x2C; the add-on array sits at +0x48 (count +0x50), the patch record at +0x58. Replay keeps four move type words instead of eight.',
        members=[
            (0, 'duration', 'float'),
            (4, 'rate', 'float'),
            (8, 'flags', 'unsigned __int64'),
            (16, 'moveTypes', 'unsigned int[4]'),
            (32, 'aimState', 'scr_string_t'),
            (36, 'aimAlias', 'scr_string_t'),
            (40, 'moveTurnState', 'scr_string_t'),
            (44, 'moveTurnAlias', 'scr_string_t'),
            (48, 'aimBlendTime', 'float'),
            (52, 'moveTurnBlendTime', 'float'),
            (56, 'lerpStrengthMin', 'float'),
            (60, 'lerpStrengthMax', 'float'),
            (64, 'customAnimHandlerType', 'PlayerCustomAnimHandlerType'),
            (72, 'aliasAddons', 'PlayerAnimsetAddonEntry *'),
            (80, 'numAliasAddons', 'unsigned int'),
            (88, 'patchInfo', 'PlayerAnimsetAliasPatch'),
            (120, 'fingerPoseSlot', 'PlayerFingerPoseSlot'),
            (121, 'padding', 'unsigned __int8[12]'),
        ]),
    dict(
        name='Replay::PlayerConditionMask',
        kind='struct',
        size=48,
        evidence='PlayerAnimsetState +0: 48 bytes in Replay, 80 in the game test build (PlayerASM_ConditionMask: two flags, then the animConditions and animEvents bit arrays). The split between the two arrays in Replay is not verified, so they stay one block of bits. Stock zones set bits in it (for example byte 8 = 0x20).',
        members=[
            (0, 'invalidMask', 'bool'),
            (1, 'usesNotetrackEvents', 'bool'),
            (4, 'bits', 'unsigned int[11]'),
        ]),
    dict(
        name='Replay::AttProjectile',
        kind='struct',
        size=240,
        evidence='q_Load_AttProjectile (RVA 0xE1EDB0) loads 0xF0: the model at +0x28 and every effect and sound 24 bytes before its game-test offset. The ten leading values (not read by the loader) keep the game-test order without the six stepped explosion values (inferred). The Replay WeaponDef of the ACTS schema also has useSteppedExplosionDamage without the six stepped values.',
        members=[
            (0, 'explosionRadius', 'int'),
            (4, 'explosionForceRadius', 'int'),
            (8, 'explosionInnerDamage', 'int'),
            (12, 'explosionOuterDamage', 'int'),
            (16, 'damageConeAngle', 'float'),
            (20, 'projectileSpeed', 'int'),
            (24, 'projectileSpeedUp', 'int'),
            (28, 'projectileActivateDist', 'int'),
            (32, 'projectileLifetime', 'float'),
            (36, 'projectileLifetimeStdDeviation', 'float'),
            (40, 'projectileModel', 'XModel *'),
            (48, 'projExplosionType', 'weapProjExposion_t'),
            (56, 'projExplosionEffect', 'FxCombinedDef'),
            (64, 'projExplosionEffectForceNormalUp', 'bool'),
            (72, 'projExplosionSound', 'SndAliasLookup'),
            (80, 'projExplosionReflClass', 'unsigned int'),
            (88, 'projDudEffect', 'FxCombinedDef'),
            (96, 'projTimeoutEffect', 'FxCombinedDef'),
            (104, 'projDudSound', 'SndAliasLookup'),
            (112, 'projTimeoutSound', 'SndAliasLookup'),
            (120, 'disableFxOnTimeout', 'bool'),
            (121, 'projImpactExplode', 'bool'),
            (122, 'disableProjectileCrumpleCheck', 'bool'),
            (124, 'destabilizationRateTime', 'float'),
            (128, 'destabilizationCurvatureMax', 'float'),
            (132, 'destabilizeDistance', 'int'),
            (136, 'vmProjBodyEffect', 'FxCombinedDef'),
            (144, 'projBodyEffect', 'FxCombinedDef'),
            (152, 'projTrailEffect', 'FxCombinedDef'),
            (160, 'projBeaconEffect', 'FxCombinedDef'),
            (168, 'projIgnitionDelay', 'int'),
            (176, 'projIgnitionEffect', 'FxCombinedDef'),
            (184, 'projIgnitionSound', 'SndAliasLookup'),
            (192, 'stickiness', 'WeapStickinessType'),
            (196, 'fuseTime', 'int'),
            (200, 'aiFuseTime', 'int'),
            (204, 'stickToPlayers', 'bool'),
            (205, 'stickToVehicles', 'bool'),
            (206, 'stickToTurrets', 'bool'),
            (207, 'stickToNonStick', 'bool'),
            (208, 'timedDetonation', 'bool'),
            (209, 'isPredictedProjectile', 'bool'),
            (210, 'useSteppedExplosionDamage', 'bool'),
            (212, 'clientTrajectoryBlendOutTime', 'int'),
            (216, 'serverTrajectoryBlendInTime', 'int'),
            (220, 'reactiveMotionRadiusScale', 'float'),
            (224, 'reactiveMotionFrequencyScale', 'float'),
            (228, 'reactiveMotionAmplitudeScale', 'float'),
            (232, 'reactiveMotionFalloff', 'float'),
            (236, 'reactiveMotionLifetime', 'float'),
        ]),
    dict(
        name='Replay::SndMusicAsset',
        kind='struct',
        size=120,
        evidence='The music set loader (RVA 0xDF46F0) strides the states 0x150 and loads the loop assets at +0x138 (count +0x134, 0x78 bytes each). The asset drops one 4-byte value (the last one, sampleRate in the game test build, inferred); the state drops the runtime reference count and status. mp_shipment and mp_frontend3 hold no music sets to check this with.',
        members=[
            (0, 'alias', 'char[64]'),
            (64, 'aliasId', 'unsigned int'),
            (68, 'inactive', 'bool'),
            (69, 'completeLoop', 'bool'),
            (70, 'removeAfterPlay', 'bool'),
            (71, 'firstRandom', 'bool'),
            (72, 'startSync', 'bool'),
            (73, 'stopSync', 'bool'),
            (74, 'completeOnStop', 'bool'),
            (76, 'loopStartOffset', 'unsigned int'),
            (80, 'bpm', 'int'),
            (84, 'assetType', 'int'),
            (88, 'loopNumber', 'int'),
            (92, 'sampleCount', 'unsigned int'),
            (96, 'startDelayBeats', 'int'),
            (100, 'startFadeBeats', 'int'),
            (104, 'stopDelayBeats', 'int'),
            (108, 'stopFadeBeats', 'int'),
            (112, 'startOffsetFrames', 'int'),
            (116, 'meter', 'int'),
        ]),
    dict(
        name='Replay::SndMusicState',
        kind='struct',
        size=336,
        evidence='No comment sits directly above this record in iw8_layouts.txt. The comment block above Replay::SndMusicAsset (Load music set, RVA 0xDF46F0) gives the 0x150 state stride and the loop asset layout.',
        members=[
            (0, 'name', 'char[64]'),
            (64, 'id', 'unsigned int'),
            (68, 'intro', 'Replay::SndMusicAsset'),
            (188, 'exit', 'Replay::SndMusicAsset'),
            (308, 'loopCount', 'unsigned int'),
            (312, 'loops', 'Replay::SndMusicAsset *'),
            (320, 'order', 'int'),
            (324, 'interruptPriority', 'int'),
            (328, 'persistPriority', 'int'),
            (332, 'isRandom', 'bool'),
            (333, 'isSequential', 'bool'),
            (334, 'skipPreviousExit', 'bool'),
            (335, 'wallClockSync', 'bool'),
        ]),
]

ALIASES = {
    'TransientCosts': 'Replay::TransientCosts',
    'TransientInfo': 'Replay::TransientInfo',
    'AttProjectile': 'Replay::AttProjectile',
    'PlayerAnimsetAlias': 'Replay::PlayerAnimsetAlias',
    'PlayerAnimEntry': 'Replay::PlayerAnimEntry',
    'SndMusicAsset': 'Replay::SndMusicAsset',
    'SndMusicState': 'Replay::SndMusicState',
}

# Patch lines in the source that name a record of this family: none. The WeaponAttachment patch
# (swaySettings at 576, weaponOffsetPatternScales at 584) names WeaponAttachment, not AttProjectile,
# and replay_layouts.py already owns that type. It is not applied here (see the port doc for the conflict).
PATCHES = []

SCALAR_SIZES = {
    'char': 1, 'signed char': 1, 'unsigned char': 1, '__int8': 1, 'unsigned __int8': 1, 'bool': 1,
    'short': 2, 'unsigned short': 2, '__int16': 2, 'unsigned __int16': 2, 'wchar_t': 2,
    'int': 4, 'unsigned int': 4, 'long': 4, 'unsigned long': 4, '__int32': 4, 'unsigned __int32': 4, 'float': 4,
    'double': 8, '__int64': 8, 'unsigned __int64': 8, 'long long': 8, 'unsigned long long': 8,
}


def _norm(name):
    return re.sub(r'\s+', ' ', name.strip())


def _known(types, name):
    """True when the schema defines the spelling, or it derives from a spelling it defines."""
    name = _norm(name)
    if name in types or name in SCALAR_SIZES:
        return True
    if name.endswith('*'):
        return _known(types, name[:-1])
    match = re.match(r'^(.+)\[(\d+)\]$', name)
    if match:
        return _known(types, match.group(1))
    return False


def _size(types, name):
    """Size in bytes of a known spelling. Adds the pointer or array entry that the spelling needs."""
    name = _norm(name)
    if name in types:
        return types[name]['size']
    if name in SCALAR_SIZES:
        return SCALAR_SIZES[name]
    if name.endswith('*'):
        types[name] = dict(name=name, size=8, kind='pointer', target=name[:-1].strip())
        return 8
    match = re.match(r'^(.+)\[(\d+)\]$', name)
    if match:
        element, count = match.group(1).strip(), int(match.group(2))
        size = _size(types, element) * count
        types[name] = dict(name=name, size=size, kind='array', count=count, element=element)
        return size
    raise KeyError(name)


def apply(types, profile):
    """Write the family's Replay records into types, set the aliases, and return the problems found."""
    problems = []
    overrides = profile.setdefault('type_overrides', {})
    for record in RECORDS:
        members = []
        for offset, member, type_name in record['members']:
            if _known(types, type_name):
                size = _size(types, type_name)
            else:
                problems.append(dict(record=record['name'], member=member, type=type_name,
                                     problem='member type is not defined in the schema'))
                size = 0
            members.append(dict(name=member, type=type_name, offset_bits=offset * 8, size_bits=size * 8))
        types[record['name']] = dict(name=record['name'], kind=record['kind'], size=record['size'], members=members)
    for patch_target, patch_members in PATCHES:
        target = types[patch_target]
        offsets = {offset for offset, _, _ in patch_members}
        kept = [m for m in target['members'] if m['offset_bits'] // 8 not in offsets]
        for offset, member, type_name in patch_members:
            kept.append(dict(name=member, type=type_name, offset_bits=offset * 8,
                             size_bits=_size(types, type_name) * 8))
        kept.sort(key=lambda m: m['offset_bits'])
        target['members'] = kept
    for base, target in ALIASES.items():
        overrides[base] = target
    return problems


def _target(overrides, spelled):
    """The schema type name that a member spelling refers to, after pointer, array and alias resolution."""
    name = _norm(spelled)
    while True:
        if name.endswith('*'):
            name = name[:-1].strip()
        elif name.startswith('const '):
            name = name[len('const '):]
        else:
            match = re.match(r'^(.+)\[(\d+)\]$', name)
            if not match:
                break
            name = match.group(1).strip()
    return overrides.get(name, name)


def _pool_reach(types, overrides, root):
    """Every schema type reachable from root through struct members, array elements and pointer targets."""
    seen, stack = set(), [overrides.get(root, root)]
    while stack:
        name = stack.pop()
        if name in seen or name not in types:
            continue
        seen.add(name)
        record = types[name]
        if record.get('kind') in ('struct', 'union'):
            refs = [m['type'] for m in record.get('members', [])]
        elif record.get('kind') == 'array':
            refs = [record['element']]
        elif record.get('kind') == 'pointer':
            refs = [record['target']]
        else:
            refs = []
        stack.extend(_target(overrides, ref) for ref in refs)
    return seen


def _check_record(types, record, family_names):
    """Mismatch strings for one record (empty when it is consistent with x64-zt)."""
    name = record['name']
    if name not in types:
        return ['record missing from schema']
    value = types[name]
    errors = []
    if value['size'] != record['size']:
        errors.append('size %d, x64-zt %d' % (value['size'], record['size']))
    found = [(m['offset_bits'] // 8, m['name'], m['type']) for m in value['members']]
    if found != list(record['members']):
        errors.append('member offsets, names or types differ from x64-zt')
    members = value['members']
    for index, m in enumerate(members):
        start, end = m['offset_bits'] // 8, (m['offset_bits'] + m['size_bits']) // 8
        gap_end = members[index + 1]['offset_bits'] // 8 if index + 1 < len(members) else value['size']
        if end > gap_end:
            errors.append('member %s (%d..%d) overlaps the next member or exceeds the size %d'
                          % (m['name'], start, end, gap_end))
        ref = _target({}, m['type'])
        by_value = types.get(ref, {}).get('kind') in ('struct', 'union', 'array', 'opaque') \
            and not m['type'].endswith('*')
        if by_value and index + 1 < len(members) and end != gap_end:
            owner = 'own' if ref in family_names else 'dependency'
            errors.append('%s member %s is %d bytes but x64-zt places the next member %d bytes on (type %s)'
                          % (owner, m['name'], end - start, gap_end - start, m['type']))
    return errors


def check(schema_path):
    with open(schema_path, encoding='utf-8') as handle:
        schema = json.load(handle)
    types = deepcopy(schema['types'])
    profile = deepcopy(schema['profiles']['replay-1.20'])
    problems = apply(types, profile)
    overrides = profile['type_overrides']
    family_names = {record['name'] for record in RECORDS}
    failures = ['%s: %s %s is not defined in the schema' % (p['record'], p['member'], p['type'])
                for p in problems]
    roots = [(pool['name'], pool['root_type']) for pool in profile['pools'] if pool.get('root_type')]
    reach = {pool: _pool_reach(types, overrides, root) for pool, root in roots}
    print('%-40s %6s %7s %5s %-10s %s' % ('record', 'x64', 'applied', 'mems', 'status', 'pool roots reaching it'))
    for record in RECORDS:
        errors = _check_record(types, record, family_names)
        failures.extend('%s: %s' % (record['name'], e) for e in errors)
        name = record['name']
        pools = sorted(pool for pool, seen in reach.items() if name in seen)
        applied = types[name]['size'] if name in types else -1
        print('%-40s %6d %7d %5d %-10s %s' % (name, record['size'], applied, len(record['members']),
                                             'ok' if not errors else 'MISMATCH',
                                             ', '.join(pools) if pools else '(none)'))
    print()
    for base, target in sorted(ALIASES.items()):
        pools = sorted(pool for pool, seen in reach.items() if target in seen)
        print('alias %-22s -> %-34s pool roots: %s' % (base, target, ', '.join(pools) if pools else '(none)'))
    print()
    for failure in failures:
        print('MISMATCH', failure)
    print('SELF-CHECK: %d records, %d aliases, %d patches, %d mismatches, %d undefined member types'
          % (len(RECORDS), len(ALIASES), len(PATCHES), len(failures) - len(problems), len(problems)))
    return 1 if failures else 0


if __name__ == '__main__':
    if '--check' not in sys.argv[1:]:
        print('usage: python -I scripts/mw19/replay_layouts_x64zt_misc.py --check')
        sys.exit(2)
    repo = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
    sys.exit(check(os.path.join(repo, 'config', 'data', 'mw19', 'schema.json')))
