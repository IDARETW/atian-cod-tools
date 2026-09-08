"""Explicit source evidence for derived runtime references and paired extents.

These annotations preserve addresses and identify the serialized representation
when one exists. They must not be used as a catch-all for unresolved pointers.
"""
from copy import deepcopy


def apply(rules, types):
    # Native DF4880 reads 16 * (m_NumPoints - 1) bytes, not the
    # decompiler's incorrectly parenthesized (16 * m_NumPoints - 1).
    if 'TacticalGraph' in types:
        rules.setdefault('TacticalGraph', {})['m_VisGraph'] = dict(
            count=dict(op='sub', args=[dict(owner='TacticalGraph', path=['m_NumPoints']), 1]),
            evidence='native_replay_loader', native_rva=0xdf4880,
            expression='m_NumPoints - 1')
        rules.setdefault('TacVisGraphRow', {})['m_Vis'] = dict(
            count=dict(op='add', args=[dict(op='shr', args=[dict(op='sub', args=[
                dict(owner='TacVisGraphRow', path=['m_End']),
                dict(owner='TacVisGraphRow', path=['m_Start'])]), 3]), 1]),
            evidence='game_test_integer_bitset_extent', source_file='archive_impl_tactical_graph_db.h',
            source_line=2221, expression='((m_End - m_Start) >> 3) + 1')
    references = [
        ('TTFDef', 'ftFace', 'FreeType face created by FT_New_Memory_Face from the font file',
         'gfx_d3d/r_fontcache.cpp', 496, 'TTFDef.file'),
        ('XModelLodInfo', 'surfs', 'Derived surface alias assigned by DB_ModelFixup_FixXModelSurf',
         'database/db_modelfixup.cpp', 688, 'XModelLodInfo.modelSurfsStaging -> XModelSurfs.surfs'),
        ('GfxConstantBufferDesc', 'bufferData', 'Resource backing address assigned during constant-buffer creation',
         'gfx_d3d/r_buffers.cpp', 1655, 'MaterialConstantBufferDef.vsData/hsData/dsData/psData'),
        ('MaterialPipelineState', 'sourceShaders', 'Program aliases installed after pipeline creation',
         'gfx_d3d/r_material.cpp', 635, 'MaterialPipelineState.serializedShaders -> MaterialSerializedShader.program'),
        ('ClientOneshotEffectDef', 'aliasList', 'Resolved sound alias; lookup falls back to effectSound.name',
         'cgame/cg_client_side_effects.cpp', 822, 'ClientOneshotEffectDef.effectSound'),
        ('ScriptableEventSoundDef', 'soundAliasCache', 'Sound alias cache populated by client event setup',
         'scriptable/scriptable_client.cpp', 2117, 'ScriptableEventSoundDef.soundAlias'),
        ('EquipmentSoundSet', 'soundNPC', 'Sound alias resolved from equipment and movement names',
         'bgame/bg_equipment_snd.cpp', 1339, 'EquipmentSoundTable cloth, movement and equipment records'),
        ('EquipmentSoundSet', 'soundPLR', 'Sound alias resolved from equipment and movement names',
         'bgame/bg_equipment_snd.cpp', 1375, 'EquipmentSoundTable cloth, movement and equipment records'),
        ('ScriptablePartDef', 'parentPart', 'Parent back-reference assigned while fixing part definitions',
         'scriptable/scriptable_game_bg_assets.cpp', 490, 'ScriptableDef.parts and ScriptablePartDef.childParts'),
        ('SpatialPartition_TransientPopulation', 'userData', 'Callback owner set to the containing DynEntityList',
         'dynentity/dynentity_load_obj.cpp', 111, 'DynEntityList'),
        ('pathnode_transient_t', 'pNextOpen', 'Transient path search list link; reset on node initialization',
         'game/pathnode.cpp', 2870, None),
        ('pathnode_transient_t', 'pPrevOpen', 'Transient path search list link; reset on node initialization',
         'game/pathnode.cpp', 2871, None),
        ('pathnode_transient_t', 'pParent', 'Transient path search parent; reset on node initialization',
         'game/pathnode.cpp', 2872, None),
        ('nav_resource_s', 'pSpace', 'Navigation space assigned when the resource is installed',
         'game/nav_space.cpp', 571, None),
        ('nav_resource_s', 'pPrev', 'Runtime resource-list link removed when the navigation space is cleared',
         'game/nav_space.cpp', 756, None),
        ('nav_resource_s', 'pNext', 'Runtime resource-list link removed when the navigation space is cleared',
         'game/nav_space.cpp', 757, None),
        ('bfx::AreaHandle', 'm_handleImpl', 'BFX area handle owned and released by the tactical graph runtime',
         'game/ai/tactical_graph.cpp', 542, None),
        ('bfx::AreaHandle', 'm_pSpace', 'Space association inside a BFX area handle released by the tactical graph runtime',
         'game/ai/tactical_graph.cpp', 542, None),
        ('scr_animtree_t', 'anims', 'Compiled animation tree returned by Scr_FindAnimTree',
         'bgame/bg_animset_util.cpp', 841, None),
        ('ScriptableDef', 'animationTreeDef', 'Compiled animation trees used to create client and server runtime trees',
         'scriptable/scriptable_game_sv.cpp', 561, 'XAnimParts assets and ScriptableDef animation events'),
        ('ScriptableEventStateChangeDef', 'part', 'Resolved part reference; part definitions are owned by the ScriptableDef hierarchy',
         'scriptable/scriptable_game_bg_assets.cpp', 419, 'ScriptableEventStateChangeDef.partReference and ScriptableDef.parts'),
        ('ScriptableEventPartDamageDef', 'part', 'Resolved part reference; part definitions are owned by the ScriptableDef hierarchy',
         'scriptable/scriptable_game_bg_assets.cpp', 419, 'ScriptableEventPartDamageDef.partReference and ScriptableDef.parts'),
        ('ScriptableEventChunkDynentDef', 'part', 'Resolved part reference; part definitions are owned by the ScriptableDef hierarchy',
         'scriptable/scriptable_game_bg_assets.cpp', 419, 'ScriptableEventChunkDynentDef.partReference and ScriptableDef.parts'),
        ('StaticModelCollisionCompressedModel', 'shapes', 'Havok shape aliases cached from physics and detail-collision assets',
         'staticmodels/staticmodels.cpp', 560, 'StaticModelCollisionCompressedModel.physicsAsset/detailCollision'),
        ('GfxWorld', 'umbraTome', 'Umbra runtime object handled by Load_UmbraTome using the serialized tome buffer',
         'gfx_d3d/r_umbra_dpvs.cpp', 539, 'GfxWorld.umbraTomeData'),
        ('SpatialPartition_Population', 'userData', 'Population callback context initialized separately from the tree and buckets',
         'spatialpartition/spatialpartition_population_test.cpp', 286, None),
        ('GfxPortalWritable', 'queuedParent', 'Back-reference in writable portal traversal state; the portal loader only fixes vertices',
         'archive_impl_r_bsp_db.h', 25578, 'GfxWorld.cells -> GfxCell.portals'),
    ]
    for owner, member, reason, file, line, serialized in references:
        if owner not in types:
            continue
        value = dict(runtime=reason, evidence='game_test_runtime_assignment_or_use', source_file=file, source_line=line)
        if owner == 'GfxPortalWritable':
            value['evidence'] = 'game_test_writable_state_type_and_loader'
        elif owner == 'SpatialPartition_Population':
            value['evidence'] = 'game_test_callback_context_type_and_test'
        if serialized:
            value['serialized_source'] = serialized
        member_type = next(m['type'] for m in types[owner]['members'] if m['name'] == member)
        t = types[member_type]
        if t['kind'] == 'array':
            assert types[t['element']]['kind'] == 'pointer'
            value = dict(elements=[deepcopy(value) for _ in range(t['count'])])
        else:
            assert t['kind'] == 'pointer'
        assert member not in rules.get(owner, {}), (owner, member, 'already has a serialization extent')
        rules.setdefault(owner, {})[member] = value

    # These public field/count pairs come from the game-test type library.
    # Their evidence is intentionally distinct from a recovered native loader.
    for owner, member, count in [('DLogSchema', 'vars', 'varCount'),
                                  ('GfxIESProfile', 'lookupTable', 'lookupTableSize'),
                                  ('bitarray_dynamic', 'array', 'wordCount'),
                                  ('GfxPortalWritable', 'hullPoints', 'hullPointCount')]:
        if owner not in types:
            continue
        assert member not in rules.get(owner, {})
        rules.setdefault(owner, {})[member] = dict(count=dict(owner=owner, path=[count]),
            evidence='game_test_type_count_pair', expression=f'{owner}.{count}')

    if 'GfxCellTree' in types:
        rules.setdefault('GfxCellTree', {})['aabbTree'] = dict(
            count=dict(indexed_count=dict(owner='GfxWorldDrawCells', items='aabbTrees', item_type='GfxCellTree',
                counts='aabbTreeCounts', count_path=['aabbTreeCount'],
                limit=dict(owner='GfxWorldDrawCells', path=['cellCount']))),
            evidence='game_test_parallel_cell_count_records_and_native_replay_loaders',
            source_file='archive_impl_r_bsp_db.h', source_line=29865,
            native_owner_loader_rva=0xd96bb0, native_item_loader_rva=0xd908e0)

    # The loader serializes views 0..31. The renderer also accepts view 32,
    # which must use its actual runtime extent when the slot is populated.
    # This is renderer evidence, not an invented extra serialization record.
    for field, count in [('smodelVisData', 'smodelVisDataCount'), ('surfaceVisData', 'surfaceVisDataCount')]:
        if 'GfxWorldDpvsStatic' not in types:
            continue
        slots = rules['GfxWorldDpvsStatic'][field]['elements']
        assert len(slots) == 33 and slots[32] is None
        slots[32] = dict(count=dict(owner='GfxWorldDpvsStatic', path=[count]),
            evidence='game_test_renderer_visibility_extent', source_file='gfx_d3d/r_dpvs.cpp', source_line=6511)
    if 'GfxWorldDpvsDynamic' in types:
        for row in range(2):
            slots = rules['GfxWorldDpvsDynamic']['dynEntVisData']['elements'][row]['elements']
            assert len(slots) == 33 and slots[32] is None
            slots[32] = dict(count=dict(op='mul', args=[32, dict(owner='GfxWorldDpvsDynamic', path=['dynEntClientWordCount', row])]),
                evidence='game_test_renderer_visibility_extent', source_file='gfx_d3d/r_umbra_dpvs.cpp',
                source_line=1000 if row == 0 else 768)
