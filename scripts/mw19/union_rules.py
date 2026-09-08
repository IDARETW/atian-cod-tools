"""Explicit union selectors recovered from asset serialization routines."""
import re


def field(owner, member): return dict(owner=owner, path=[member])
def op(name, left, right): return dict(op=name, args=[left, right])
def present(owner): return dict(context_exists=owner)
def select(condition, yes, no): return dict(op='select', args=[condition, yes, no])


def switch_arms(code, members, values):
    """Map a case only when its body binds exactly one declared union type.

    Numeric enum values are supplied by the type library, never the order of
    members or cases. Ambiguous/changed source shapes stop regeneration.
    """
    arms = []
    for match in re.finditer(r'case (\w+)\s*:(.*?)(?=\bbreak;)', code, re.S):
        label, body = match.groups()
        if re.search(r'\bcase\s', body):
            raise ValueError('Grouped cases require explicit handling: ' + label)
        candidates = [m for m in members if re.search(r'\bvar' + re.escape(m['type']) + r'\s*=', body)]
        if len(candidates) != 1:
            raise ValueError(f'Ambiguous union case {label}: {candidates}')
        arms.append((values[label], candidates[0]['name'], {}))
    return arms


def generate(sources, types):
    rules = {}

    def add(name, selector, arms, evidence='game_test_loader', source_name=None):
        source = sources[source_name or name]
        declared = {m['name'] for m in types[name]['members']}
        assert all(member in declared for _, member, _ in arms), name
        assert len({value for value, _, _ in arms}) == len(arms), name
        rules[name] = dict(selector=selector, cases=[dict(value=value, member=member, **details) for value, member, details in arms],
                           evidence=evidence, source_file=source['file'], source_line=source['line'])

    add('XAnimIndices', op('ge', field('XAnimParts', 'numframes'), 256), [
        (0, '_1', dict(count=field('XAnimParts', 'indexCount'))),
        (1, '_2', dict(count=field('XAnimParts', 'indexCount')))])
    add('XSurfaceSharedData', op('and', field('XSurfaceShared', 'flags'), 1), [
        (0, 'residentData', dict(count=field('XSurfaceShared', 'dataSize'), required_if_nonempty=True)),
        (1, 'streamedDataHandle', dict(external=True))], 'native_replay_E2AFA0_and_game_test_loader')
    add('GfxImagePixels', op('and', field('GfxImage', 'flags'), 64), [
        (0, 'residentData', dict(count=field('GfxImage', 'totalSize'), required_if_nonempty=True)),
        (64, 'streamedDataHandle', dict(external=True))], 'native_replay_E2FFF0_and_game_test_loader')
    for name, owner, mode, ai, player in [
        ('ASM_Union', 'ASM', 'm_Mode', 'm_AIASM', 'm_PlayerASM'),
        ('ASM_State_Union', 'ASM', 'm_Mode', 'm_AIState', 'm_PlayerState'),
        ('Animset_Union', 'Animset', 'mode', 'm_AIAnimset', 'm_PlayerAnimset'),
        ('AnimsetState_Union', 'Animset', 'mode', 'm_AIAnimsetState', 'm_PlayerAnimsetState'),
        ('AnimsetAlias_Union', 'Animset', 'mode', 'm_AIAnimsetAlias', 'm_PlayerAnimsetAlias')]:
        add(name, field(owner, mode), [(0, ai, dict(count=1)), (1, player, dict(count=1))])
    for name, compressed, uncompressed in [
        ('MayhemFramesUnion', 'splineCompressedFrames', 'uncompressedFrames'),
        ('MayhemDataChannelsUnion', 'splineCompressedKeys', 'uncompressedKeys')]:
        add(name, op('ne', field('MayhemAnim', 'isSplineCompressed'), 0),
            [(0, uncompressed, dict(count=1)), (1, compressed, dict(count=1))])

    for name, owner, member, enum, initial_label, initial_member, expected in [
        ('ScriptableEventDefUnion', 'ScriptableEventDef', 'type', 'Scriptable_EventType',
         'Scriptable_EventType_StateChange', 'stateChange', 44),
        ('ScriptableStateDefUnion', 'ScriptableStateDef', 'type', 'Scriptable_StateType',
         'Scriptable_StateType_Simple', 'simple', 4),
        ('PhysicsSFXEventAssetRuleUnion', 'PhysicsSFXEventAssetRule', 'type', 'PhysicsSFXEventAssetRuleType',
         'Types_PhysicsSFXEventSoundRule', 'soundRule', 9),
        ('PhysicsVFXEventAssetRuleUnion', 'PhysicsVFXEventAssetRule', 'type', 'PhysicsVFXEventAssetRuleType',
         'Types_PhysicsVFXEventParticleEffectRule', 'particleEffectRule', 9),
        ('ParticleModuleTypeDef', 'ParticleModuleDef', 'moduleType', 'ParticleModuleType',
         'PARTICLE_MODULE_INIT_ATLAS', 'initAtlas', 62)]:
        code = sources[name]['code']
        values = types[enum]['values']
        assert f'var{owner}->{member}' in code
        initial_type = next(m['type'] for m in types[name]['members'] if m['name'] == initial_member)
        assert re.search(r'\bvar' + initial_type + r'\s*=', code), name
        arms = [(values[initial_label], initial_member, {})] + switch_arms(code, types[name]['members'], values)
        assert len(arms) == expected, (name, len(arms), expected)
        add(name, field(owner, member), arms)

    model_values = types['ScriptableDataType']['values']
    add('ScriptableModelUnion', field('ScriptableEventModelDef', 'dataType'), [
        (model_values['SCRIPTABLE_DATA_TYPE_XMODEL'], 'model', {}),
        (model_values['SCRIPTABLE_DATA_TYPE_XCOMPOSITEMODEL'], 'compositeModel', {})])
    # Bit 1 denotes resident data; bit 0 chooses the serialization stream.
    add('StreamKeyData', op('and', field('StreamKey', 'flags'), 3), [
        (0, 'dataHandle', dict(external=True)), (1, 'dataHandle', dict(external=True)),
        (2, 'residentData', dict(count=field('StreamKey', 'dataSize'), required_if_nonempty=True)),
        (3, 'residentDataGPU', dict(count=field('StreamKey', 'dataSize'), required_if_nonempty=True))],
        'native_replay_E34050_and_game_test_loader')
    # Inactive keys retain their asset hash; active known behaviors use a
    # runtime user context. Native 13ACBB0 clears both on release. Preserve
    # the user pointer without traversing a handler's private runtime object.
    context_union = next(m['type'] for m in types['StreamKey']['members'] if m['name'] == '___u3')
    behavior_values = types['StreamKeyBehaviorIndex']['values']
    contexts = [(0, 'assetHash', {})]
    for label, value in behavior_values.items():
        if label not in ('SKBI_NONE', 'SKBI_COUNT'):
            contexts.append((value, 'behaviorUserPtr', dict(runtime=f'{label} runtime behavior user context')))
    add(context_union, field('StreamKey', 'behaviorIndex'), contexts,
        'native_replay_13ACBB0_and_game_test_behavior_release', source_name='StreamKey')
    rules[context_union].update(source_file='stream/stream_key_behavior.cpp', source_line=240)

    for name, owner in [('XAnimDeltaPartQuatData', 'XAnimDeltaPartQuat'),
                        ('XAnimDeltaPartQuatData2', 'XAnimDeltaPartQuat2'),
                        ('XAnimPartTransData', 'XAnimPartTrans')]:
        add(name, op('ne', field(owner, 'size'), 0), [(0, 'frame0', {}), (1, 'frames', {})])
    translation_count = select(op('ne', field('XAnimPartTrans', 'size'), 0),
                               op('add', field('XAnimPartTrans', 'size'), 1), 0)
    add('XAnimDynamicFrames', op('ne', field('XAnimPartTrans', 'smallTrans'), 0), [
        (0, '_2', dict(count=translation_count)), (1, '_1', dict(count=translation_count))])
    # These arrays are inline tails, not pointers. Their allocation extends
    # beyond the one-element declaration recorded in the type library.
    dynamic_count = op('add', select(present('XAnimPartTrans'), field('XAnimPartTrans', 'size'),
                           select(present('XAnimDeltaPartQuat'), field('XAnimDeltaPartQuat', 'size'),
                                  field('XAnimDeltaPartQuat2', 'size'))), 1)
    add('XAnimDynamicIndices', op('ge', field('XAnimParts', 'numframes'), 256), [
        (0, '_1', dict(inline_count=dynamic_count)), (1, '_2', dict(inline_count=dynamic_count))],
        source_name='XAnimDynamicIndicesDeltaQuat')

    particle_values = types['ParticleModuleType']['values']
    arms = []
    for label, member in [
        ('INIT_MATERIAL', 'material'), ('INIT_MODEL', 'model'), ('INIT_SPAWN_SHAPE_MESH', 'model'),
        ('PHYSICS_LIGHT', 'physicsFXData'), ('INIT_PARTICLE_SIM', 'particleSim'), ('INIT_DECAL', 'decal'),
        ('INIT_SOUND', 'sound'), ('INIT_VECTOR_FIELD', 'vectorField'),
        ('INIT_LIGHT_OMNI', 'lightDef'), ('INIT_LIGHT_SPOT', 'lightDef'),
        ('INIT_BEAM', 'particleSystem'), ('INIT_RUNNER', 'particleSystem')]:
        arms.append((particle_values['PARTICLE_MODULE_' + label], member, dict(string=True) if member == 'sound' else {}))
    # The source loader's default path also accepts links in a test-event
    # handler. Only unhandled module values use that path.
    known = {value for value, _, _ in arms}
    # Preserve the explicit cases even when a handler context is active.
    unmatched = 1
    for value in sorted(known):
        unmatched = op('and', unmatched, op('ne', field('ParticleModuleDef', 'moduleType'), value))
    linked_selector = select(op('and', present('ParticleModuleTestEventHandlerData'), unmatched),
                             65536, field('ParticleModuleDef', 'moduleType'))
    arms.append((65536, 'particleSystem', {}))
    add('ParticleLinkedAssetDef', linked_selector, arms)
    add('pathnode_tree_info_t', op('and', field('pathnode_tree_t', 'axis'), 0x80000000), [
        (0, 'child', dict(element_count=1)), (0x80000000, 's', {})])
    # Both ScriptBundle views designate the same serialized allocation. The
    # source routine always reads rootSize 32-bit words; jsonblob is not an
    # ordinary null-terminated string despite its char* declaration.
    add('$EE658B4838C130B7503228AEB3560558', 0,
        [(0, 'root', dict(count=field('ScriptBundle', 'rootSize')))], source_name='ScriptBundle')
    return rules
