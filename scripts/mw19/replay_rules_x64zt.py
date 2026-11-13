"""Pointer rules for Replay records ported from x64-zt tools/iw8_layouts.txt.

Traversal looks up rules by the native (Replay::) type name first, then by the source name
(mw19_schema.cpp Value()), so these Replay-keyed rules take precedence over game-test rules.
Counts follow the Replay loaders as recorded by x64-zt (iw8_layouts.txt comments and the
loader op graph in tools/iw8_format.json, both derived from game_dx12_ship_replay.exe 1.20.4.7623265).
"""

GPU_RUNTIME = ('x64-zt iw8_layouts.txt: GPU buffer records are copied as bytes by Load_GfxWrapped*Buffer / '
               'Load_GfxIndexBuffer (no pointer fixups); the renderer fills them at runtime')
TECHNIQUE = 'Load_MaterialTechnique RVA 0xE0CED0 (x64-zt iw8_layouts.txt, iw8_format.json op graph)'


def rules(types):
    """Return {owner: {member: rule}} for the ported Replay records present in `types`."""
    out = {}

    def runtime(owner, member, what):
        if owner in types and any(m['name'] == member for m in types[owner].get('members', [])):
            out.setdefault(owner, {})[member] = dict(runtime=what, evidence=GPU_RUNTIME)

    for owner in ('Replay::GfxBufferResource', 'Replay::GfxIndexBuffer', 'Replay::GfxShaderBufferView',
                  'Replay::GfxShaderBufferRWView', 'Replay::GfxVertexBuffer'):
        runtime(owner, 'resource', 'ID3D12Resource')
    runtime('Replay::GfxWrappedBuffer', 'data', 'mapped GPU buffer')
    if 'Replay::MaterialPipelineState' in types:
        out['Replay::MaterialPipelineState'] = dict(
            pso=dict(runtime='ID3D12PipelineState', evidence='Created by the renderer after load; ' + TECHNIQUE))
    if 'Replay::MaterialTechnique' in types:
        def field(name):
            return dict(owner='MaterialTechnique', path=[name])
        out['Replay::MaterialTechnique'] = dict(
            pipelineStates=dict(count=dict(op='mul', args=[field('pipelineStateCount'), field('pipelineStateInputLayoutCount')]),
                                evidence=TECHNIQUE + ': +0x83 * +0x0E records of 16 bytes at +0x28'),
            rootParameterMap=dict(count=16, evidence=TECHNIQUE + ': 16-byte map at +0x50'),
            stateBits=dict(count=field('pipelineStateCount'), evidence=TECHNIQUE + ': +0x83 records of 5 bytes at +0x88'))
    # Super-terrain node meshes. Replay::StMesh embeds these records by their Replay:: names, so the game-test
    # rules keyed StVertexData0 / StMeshLayerMaskData never match, and their game-test counts (4 * vertexCount for
    # both) are not the Replay ones anyway. The owner is the enclosing StMesh (reached as 'StMesh' from
    # StTerrainNode.mesh; the replay-1.20 type override maps it to Replay::StMesh).
    def mesh(*path):
        return dict(owner='StMesh', path=list(path))
    if 'Replay::StVertexData0' in types:
        out['Replay::StVertexData0'] = dict(data=dict(
            count=dict(op='mul', args=[mesh('vertexCount'), dict(op='select', args=[
                dict(op='and', args=[mesh('flags', 'packed'), 4]), 4, 8])]),
            evidence='Load_StVertexData0 RVA 0xE1EB60: test byte [StMesh+0x126], 4; cmovne 4 else 8; '
                     'imul by word [StMesh+0x120] (vertexCount) -> bytes at +0'))
    if 'Replay::StMeshLayerMaskData' in types:
        out['Replay::StMeshLayerMaskData'] = dict(data=dict(
            count=dict(op='mul', args=[dict(op='select', args=[
                dict(op='and', args=[mesh('flags', 'packed'), 16]),
                dict(op='sub', args=[mesh('triCount'), mesh('cutoutTriCount')]), 0]), 4]),
            evidence='Load_StMeshLayerMaskData RVA 0xE1E390: test byte [StMesh+0x126], 0x10; if set '
                     '(word [+0x122] triCount - word [+0x124] cutoutTriCount) * 4 bytes, else 0'))
    return out
