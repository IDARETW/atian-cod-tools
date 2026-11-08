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
    return out
