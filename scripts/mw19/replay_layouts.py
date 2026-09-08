"""Replay layout differences established from native load routines.

Offsets are bytes. Unknown runtime fields remain opaque and are not assigned
invented C++ names. Source-derived scalar semantics remain identified as such.
"""
from copy import deepcopy


def apply(types, profile):
    overrides = profile.setdefault('type_overrides', {})
    source_names = list(types)

    def clone(name, size):
        target = 'Replay::' + name
        value = deepcopy(types[name])
        value.update(name=target, size=size)
        types[target] = value
        overrides[name] = target
        return value

    def member(name, type, offset, size):
        return dict(name=name, type=type, offset_bits=offset * 8, size_bits=size * 8)

    def opaque(name, size, reason):
        types[name] = dict(name=name, kind='opaque', size=size, reason=reason)
        return name

    def mapped(pool, root, loader, note):
        record = next(p for p in profile['pools'] if p['name'] == pool)
        record.update(root_type=root, status='native_pointer_layout', layout_note=note, loader_rva=loader)

    # Native stream routines 139F7A0/13A3280 bind the same 232-byte image
    # pool as F5F290, but read streamedPartCount at +50, not source +49.
    # Byte +49 is kept explicitly unnamed until its semantics are verified.
    image = clone('GfxImage', 232)
    for m in image['members']:
        if m['name'] == 'streamedPartCount': m['offset_bits'] = 50 * 8
        elif m['name'] == 'decalAtlasIndex': m.update(name='unknown_0x31', offset_bits=49 * 8)
    image['members'].sort(key=lambda m: m['offset_bits'])

    # Load_Camo at E411C0 ends after the texture array. The source's two
    # vehicle VFX references at A8 and B0 are absent in this Replay build.
    camo = clone('Camo', 168)
    camo['members'] = [m for m in camo['members'] if m['offset_bits'] < 168 * 8]
    mapped('camo', 'Camo', 0xE411C0, 'Native name, script string and texture array; source scalar fields through blendMapChannels.')

    # E30980 loads exactly one 56-byte AlwaysloadedFlagSet at +8, then the
    # optional TransientInfo at +40. Game-test instead has three flag sets.
    streaming = clone('StreamingInfo', 72)
    types['AlwaysloadedFlagSet[1]'] = dict(name='AlwaysloadedFlagSet[1]', kind='array',
                                         size=56, count=1, element='AlwaysloadedFlagSet')
    streaming['members'] = [member('name', 'const char *', 0, 8),
                            member('alwaysloadedFlagSets', 'AlwaysloadedFlagSet[1]', 8, 56),
                            member('transientInfo', 'TransientInfo *', 64, 8)]
    mapped('streaminginfo', 'StreamingInfo', 0xE30980, 'One native flag set, with transientInfo at 0x40.')

    # All six DX12 shader assets have name/debugName, one runtime shader
    # handle, and a 16-byte program load definition. E0A8E0/E0AE10/E0AF60
    # read its bytecode pointer at +0 and byte count at +8.
    program = clone('ComputeShaderProgram', 24)
    program['members'] = [m for m in program['members'] if m['name'] != 'derivedCS']
    next(m for m in program['members'] if m['name'] == 'loadDef')['offset_bits'] = 64
    compute = clone('ComputeShader', 40)
    next(m for m in compute['members'] if m['name'] == 'prog')['size_bits'] = 24 * 8
    for name in ('computeshader', 'libshader', 'vertexshader', 'hullshader', 'domainshader', 'pixelshader'):
        record = next(p for p in profile['pools'] if p['name'] == name)
        record.update(root_type='ComputeShader', status='native_pointer_layout',
                       layout_note='Native 40-byte shader layout; prog.cs is the stage-specific runtime handle.')

    # The remainder of Load_XModel is outside its SQLite function boundary.
    # Native instructions E29D94..E2A12F establish unchanged members through
    # blendShapeInfo, then mdaoVolumes +2A0 and decalVolumesInfo +2A8.
    model = clone('XModel', 688)
    for m in model['members']:
        if m['name'] in ('mdaoVolumes', 'decalVolumesInfo'): m['offset_bits'] += 64
    model['members'].insert(-2, member('runtime_0x298', opaque('Replay::XModelRuntimeWord', 8,
                              'Not traversed by the native asset loader'), 664, 8))
    mapped('xmodel', 'XModel', 0xE29820, 'Native pointer offsets include the loader tail through RVA 0xE2A12F; extra runtime word at 0x298.')

    # Native GPU records differ across these builds. Their Load_Stream
    # routines contain no pointer fixups; preserve their bytes as runtime
    # records and propagate the native extents into containing structures.
    for name, size in [('GfxWrappedBuffer', 64), ('GfxWrappedRWBuffer', 96), ('GfxShaderBufferView', 24)]:
        overrides[name] = opaque('Replay::' + name, size, 'Native GPU runtime record; loader copies bytes without serialized pointer fixups')

    effects = clone('ClientSideEffects', 120)
    effects['members'] = [m for m in effects['members'] if m['offset_bits'] < 120 * 8]
    # E305D0 fixes surfaces/bounds/materials/data at 28/30/38/40 hex,
    # followed by the 64-byte GPU buffer. Replay has none of the source's
    # himip arrays or transient-zone surface indices.
    surfaces = clone('GfxWorldSurfaces', 136)
    surfaces['members'] = [m for m in surfaces['members'] if m['offset_bits'] < 40 * 8] + [
        member('surfaces', 'GfxSurface *', 40, 8),
        member('surfaceBounds', 'GfxSurfaceBounds *', 48, 8),
        member('surfaceMaterials', 'GfxDrawSurf *', 56, 8),
        member('surfData', 'GfxWorldSurfData *', 64, 8),
        member('surfDataBuffer', 'GfxWrappedBuffer', 72, 64)]
    # D9C260 copies 776 bytes and fixes aliases through +768. The last
    # two DLC pairs in the game-test record do not exist in Replay.
    sounds = clone('WeaponSFXPackageSounds', 776)
    sounds['members'] = [m for m in sounds['members'] if m['offset_bits'] < 776 * 8]
    # D9A580 fixes the bounce arrays at 1904/1912, the ignition effect at
    # 1976 and patterns at 3048. The six source stepped-explosion integers
    # are absent. D26520 independently selects the six unchanged 32-byte
    # curves beginning at 2852. In the loader tail, BallisticInfo +5208 is
    # followed directly by notifyTypes +5248, without HyperBurstInfo.
    weapon = clone('WeaponDef', 5296)
    weapon['members'] = [m for m in weapon['members']
                         if not m['name'].startswith('iExplosionStepped') and m['name'] != 'hyperBurstInfo']
    for m in weapon['members']:
        offset = m['offset_bits'] // 8
        if offset >= 1916: m['offset_bits'] -= 24 * 8
        if offset >= 5288: m['offset_bits'] -= 16 * 8
    # D96710 proceeds directly from lightmapTransientIndex to volumetrics;
    # the source's displacement parameter/count/GPU fields are absent.
    draw = clone('GfxWorldDraw', 12760)
    draw['members'] = [m for m in draw['members'] if not 0x3148 * 8 <= m['offset_bits'] < 0x3170 * 8]
    for m in draw['members']:
        if m['offset_bits'] >= 0x3170 * 8: m['offset_bits'] -= 40 * 8
    attachment = clone('WeaponAttachment', 968)
    attachment['members'] = [m for m in attachment['members'] if m['name'] != 'weaponOffsetPatternScaleInfo']
    for m in attachment['members']:
        if m['offset_bits'] >= 600 * 8: m['offset_bits'] -= 64
    terrain_surface = clone('StDiskTerrainSurface', 400)
    index_buffer = opaque('Replay::GfxIndexBuffer', 32, 'Native index buffer runtime record, Load_Stream at RVA 0xDD1770')
    for m in terrain_surface['members']:
        if m['name'] == 'highLODIndexBuffer': m.update(type=index_buffer, size_bits=32 * 8)
        elif m['offset_bits'] >= 24 * 8: m['offset_bits'] += 24 * 8

    # Each substitution retains the original gaps and moves later members by
    # the size difference of preceding embedded records. Pointer widths stay
    # eight bytes. Independent native root sizes/offsets below guard this step.
    for iteration in range(32):
        changed = False
        for name in source_names:
            current = types[overrides.get(name, name)]
            if current['kind'] == 'array':
                element = current['element']
                size = types[overrides.get(element, element)]['size'] * current['count']
                if size != current['size']:
                    if name not in overrides: current = clone(name, current['size'])
                    current['size'] = size
                    changed = True
            elif current['kind'] == 'struct':
                updated, shift = [], 0
                for original in current['members']:
                    m = deepcopy(original)
                    target = m['type']
                    native = types[overrides.get(target, target)]
                    delta = 0
                    if target in overrides and types[target]['kind'] in ('struct', 'array', 'opaque'):
                        delta = native['size'] * 8 - m['size_bits']
                    m['offset_bits'] += shift
                    m['size_bits'] += delta
                    shift += delta
                    updated.append(m)
                if shift:
                    if name not in overrides: current = clone(name, current['size'])
                    current.update(size=current['size'] + shift // 8, members=updated)
                    changed = True
        if not changed: break
    else: raise RuntimeError('Replay embedded layout propagation did not converge')

    def check(name, size, offsets):
        value = types[overrides.get(name, name)]
        assert value['size'] == size, (name, value['size'], size)
        found = {m['name']: m['offset_bits'] // 8 for m in value['members']}
        for field, offset in offsets.items():
            assert found[field] == offset, (name, field, found[field], offset)

    check('StClutterSamplePoints', 80, dict(samplePointBuffer=16))
    check('GfxWorldSurfaces', 136, dict(surfaces=40, surfaceBounds=48, surfaceMaterials=56,
                                     surfData=64, surfDataBuffer=72))
    check('WeaponSFXPackageSounds', 776, dict(dlcSound1Player=768))
    check('WeaponDef', 5296, dict(parallelBounce=1904, projIgnitionEffect=1976,
                                weaponOffsetCurveHoldFireSlow=2852, weaponOffsetPatterns=3048,
                                mountRumble=4968, ballisticInfo=5208, notifyTypes=5248))
    check('GfxImage', 232, dict(levelCount=48, streamedPartCount=50, streams=56, pixels=224))
    check('StTerrain', 152, dict(clutterSamplePoints=56, lightmapCount=140, lightmaps=144))
    check('StDiskTerrainSurface', 496, {})
    check('GfxWorldDrawVerts', 200, dict(posBuffer=24, auxBuffer=88, indexCount=152, indices=160, indexBuffer=168))
    check('GfxWorldTransientZone', 328, dict(drawVerts=16, drawCells=216, tempLightmapData=240, gpuLightGrid=248,
                                           reflectionProbes=288, collectionCount=296, collections=304, decalVolumes=312, compressedSunShadow=320))
    check('MapEnts', 1032, dict(clientSideEffects=768, createFxAssetData=888, exploderNames=912, mayhemScenes=944,
                              spawners=952, audioPASpeakers=976, collmapLookups=1024))
    check('GfxWorld', 17808, dict(surfaces=200, smodels=336, draw=1608, dynamicLightset=14480, mayhemSelfVis=15408,
                                frustumLights=15520, lightViewFrustums=15712, primaryLights=15720, voxelTreeCount=15728,
                                dpvs=16280, dpvsDyn=16912, numUmbraGates=17472, xmodelStreamTreeGrid=17720))
    for pool, root, loader in [('attachment', 'WeaponAttachment', 0xE0E480), ('map_ents', 'MapEnts', 0xE0B520),
                               ('gfx_map', 'GfxWorld', 0xD92E80), ('gfx_map_trzone', 'GfxWorldTransientZone', 0xD96E90),
                               ('stterrain', 'StTerrain', 0xE1EFA0)]:
        mapped(pool, root, loader, 'Native root offsets and size checked; embedded GPU record extents propagated from Replay loaders.')
