"""Generate versioned MW2019 pool maps from saved, independently acquired evidence.

No inference from enum ordinals across builds is permitted: profiles join by
native asset name. Game-test types are retained as source evidence; a matching
root size alone is not claimed to prove a Replay member layout.
"""
import argparse
import hashlib
import json
import re
import struct
from pathlib import Path
import replay_layouts
import union_rules
import replay_image

ROOT = Path(__file__).resolve().parents[2]
RESEARCH = ROOT / 'research/mw19'
DEST = ROOT / 'config/data/mw19'


def read(name):
    return json.loads((RESEARCH / name).read_text(encoding='utf-8'))


def table(document, name):
    return next(v for n, v in document['symbols'].items() if name in n)


def uints(t):
    b = bytes.fromhex(t['bytes'])
    return list(struct.unpack('<' + 'I' * (len(b) // 4), b))


def write(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    text = json.dumps(value, indent=2, ensure_ascii=True) + '\n'
    if not path.exists() or path.read_text(encoding='utf-8') != text:
        path.write_text(text, encoding='utf-8')


def game_test_schema(dev):
    types = dev['types']
    # Export only the asset type graph, not unrelated database or dvar types.
    todo = ['XAssetHeader']
    selected = {}
    while todo:
        name = todo.pop()
        if name in selected:
            continue
        src = types[name]
        dst = {k: v for k, v in src.items() if k != 'declaration'}
        selected[name] = dst
        if src['kind'] == 'pointer':
            todo.append(src['target'])
        elif src['kind'] == 'array':
            todo.append(src['element'])
        elif src['kind'] in ('struct', 'union'):
            todo.extend(m['type'] for m in src['members'])
        elif src['kind'] == 'enum':
            dst['values'] = {n: int(v, 0) for n, v in re.findall(r'(\w+)\s*=\s*(0x[0-9A-Fa-f]+|[0-9]+)', src['declaration'])}
    return selected


def main():
    dev = read('game_test_pools.json')
    native = read('native_probe2.json')['tables']
    assert dev['complete'] and not dev['errors']
    names = table(dev, 'g_assetNames')['strings']
    sizes = uints(table(dev, 'g_assetSizes'))
    aligns = uints(table(dev, 'g_assetAlignment'))
    capacities = uints(table(dev, 's_poolSize'))
    valid = bytes.fromhex(table(dev, 'g_assetNameFieldValid')['bytes'])
    members = [m for m in dev['types']['XAssetHeader']['members'] if m['name'] != 'data']
    active = [(i, n) for i, n in enumerate(names) if sizes[i]]
    assert len(members) == len(active), (len(members), len(active))
    roots = {}
    for (i, n), m in zip(active, members):
        root = dev['types'][m['type']]['target']
        assert dev['types'][root]['size'] == sizes[i], (n, root, sizes[i])
        roots[n] = root
    types = game_test_schema(dev)
    profiles = {}
    pools = []
    for i, name in enumerate(names):
        pools.append(dict(id=i, name=name, size=sizes[i], alignment=aligns[i],
                          capacity=capacities[i], name_offset=0 if valid[i] else None,
                          root_type=roots.get(name), status='source_layout' if sizes[i] else 'unused',
                          pool_rva=table(dev, 's_assetPools')['rva'] + i * 32))
    profiles['game-test'] = dict(id='game-test', module='1-game_test.exe', pool_count=len(pools),
                                entry_table_rva=table(dev, 's_assetManager@@')['rva'],
                                entry_size=20, entry_capacity=383488, allocation_flags_offset=7669768,
                                table_rvas={n: table(dev, s)['rva'] for n, s in
                                            [('names', 'g_assetNames'), ('sizes', 'g_assetSizes'),
                                             ('alignment', 'g_assetAlignment'), ('pools', 's_assetPools')]}, pools=pools)
    replay_names = native['asset_names']['strings']
    replay_sizes = uints(native['asset_sizes'])
    replay_alignments = uints(native['asset_alignment'])
    replay_capacity = uints(native['pool_capacity'])
    defaults = native['default_names']['strings']
    default_ptrs = struct.unpack('<117Q', bytes.fromhex(native['default_names']['bytes']))
    # IDA returns an empty byte string for both an empty C string and no string.
    # A non-null native pointer with no contents denotes the empty string.
    defaults = [s if s is not None else '' if ptr else None for s, ptr in zip(defaults, default_ptrs)]
    assert len(replay_names) == len(replay_sizes) == len(replay_capacity) == 117
    pools = []
    for i, name in enumerate(replay_names):
        root = roots.get(name)
        size = replay_sizes[i]
        same_size = bool(root and types[root]['size'] == size)
        pools.append(dict(id=i, name=name, size=size, alignment=replay_alignments[i],
                          capacity=replay_capacity[i], default_asset=defaults[i],
                          name_offset=0 if size else None, source_root_type=root,
                          root_type=root if same_size else None,
                          status='source_size_match' if same_size else 'layout_pending' if size else 'unused'))
    profiles['replay-1.20'] = dict(id='replay-1.20', module='game_dx12_ship_replay.exe',
                                 version='1.20.4.7623265', pool_count=117,
                                 table_rvas={n: native[s]['rva'] for n, s in
                                             [('names', 'asset_names'), ('sizes', 'asset_sizes'),
                                              ('alignment', 'asset_alignment'), ('capacity', 'pool_capacity')]},
                                 entry_table_rva=0xC815C60, entry_size=20, entry_capacity=358400,
                                 allocation_flags_offset=7168008,
                                 evidence=dict(names='native name pointer table', sizes='native size table and DB_AddXAsset copy',
                                               entries='native DB_AssetEntryPool allocation and bitmap writes'), pools=pools)
    allocs = read('alloc_functions.json')
    for p, a in zip(pools, allocs):
        p['allocator_rva'] = a['rva'] if a['va'] else None
        p['free_head_rva'] = 0xB8F4F88 + p['id'] * 24
        p['pool_pointer_rva'] = 0x23FBD80 + p['id'] * 8
    profiles['replay-1.20']['table_rvas'].update(pool_pointers=0x23FBD80, allocators=0x23FC130)
    profiles['replay-1.20']['image_layout'] = replay_image.metadata()
    profiles['replay-1.20']['technique_set_layout'] = dict(
        loader_rva=0xe0d2a0, mask_offset=24, mask_words=4, mask_word_bytes=8,
        masked_techniques_offset=56, popcount_rva=0xe0d380, pointer_loader_call_rva=0xe0d3bd)
    profiles['replay-1.20']['cell_tree_layout'] = dict(
        owner_loader_rva=0xd96bb0, tree_loader_rva=0xd908e0, owner_size=24,
        cell_count_offset=0, count_records_offset=8, trees_offset=16,
        count_record_size=4, tree_record_size=8, aabb_record_size=48,
        count_evidence='game-test parallel count records; native loaders confirm extents and bindings')
    profiles['replay-1.20']['stream_buffer_layout'] = dict(
        xpak_entry_selector_rva=0x13ad700, buffer_size_rva=0x13b3390,
        streamkey=dict(key_offset=8, data_offset=40, size_offset=56, flags_offset=61,
                       resident_mask=2, data_loader_rva=0xe34050),
        xmodelsurfs=dict(key_offset=16, shared_offset=48, count_offset=56,
                         shared_size_offset=8, shared_flags_offset=12,
                         streamed_mask=1, data_loader_rva=0xe2afa0))
    profiles['replay-1.20']['mesh_geometry_layout'] = dict(
        surface_size=192, vertex_stride=20, triangle_stride=6,
        vertex_count_offset=2, triangle_count_offset=4, shared_offset=72,
        vertex_data_offset=36, index_data_offset=40, bounds_offset=144,
        position_bits=21, position_scale='max(bounds.halfSize)',
        unpack_position_rva=0x1992d20, surface_triangle_reader_rva=0x1bb7474,
        normal_uv_evidence='game-test scalar decoder and Greyhound 2020-04-29 aa0ab50',
        geometry_scope='base surfaces; skeleton, skinning, materials, morphs and subdivision are separate')
    profiles['replay-1.20']['sound_bank_layout'] = dict(
        root_size=512, stream_key_offset=496, stream_key_loader_binding_rva=0xe39f55,
        header_size=688, version=10, build_version=16, entry_size=44,
        header_validator_rva=0x1b85ab0, entry_search_rva=0x1b859c0,
        loaded_bank_bind_rva=0x1b86070, key_offset=0, encoded_size_offset=4,
        seek_size_offset=8, frame_count_offset=12, hybrid_pcm_size_offset=16,
        data_offset_offset=20, sample_rate_offset=28, channels_offset=32,
        looping_offset=33, codec_offset=34,
        flac_codec=8, flac_evidence='Replay loose SAB sample and FLAC frame validation')
    replay_image.write_header(ROOT / 'src/core/acts/tools/mw19/mw19_image_formats.hpp')
    replay_layouts.apply(types, profiles['replay-1.20'])
    loaders = read('native_root_loaders.json')
    for p in pools:
        if p['name'] in loaders:
            p['loader_rva'] = loaders[p['name']]['loader_rva']
            p['pointer_loader_rva'] = loaders[p['name']]['pointer_loader_rva']
    schema = dict(schema_version=1, profiles=profiles, types=types)
    schema['pointer_rules'] = read('pointer_rules.json')['rules']
    schema['pointer_rules']['Replay::DDLMember'] = dict(serializedByte=dict(
        count=1, evidence='Replay DF2F70 fixes pointer +16; DD3B20 loads one byte'))
    schema['pointer_rules']['ReplayFDA::ComWorld'] = dict(
        primaryLights=dict(count=dict(owner='ReplayFDA::ComWorld', path=['primaryLightCount']), evidence='Legacy 344-byte light array'),
        umbraGateNames=dict(count=dict(owner='ReplayFDA::ComWorld', path=['numUmbraGates']), evidence='Legacy gate-name pointer array'))
    schema['union_rules'] = union_rules.generate(read('source_loaders.json'), types)
    write(DEST / 'schema.json', schema)
    rows = ['# MW2019 pool coverage', '',
            'Generated by `scripts/mw19/generate_schema.py`. Native pool IDs and sizes are independent of the game-test type library.', '',
            '| ID | Replay pool | Size | Alignment | Capacity | Root type | Layout evidence |',
            '|---:|---|---:|---:|---:|---|---|']
    for p in pools:
        rows.append(f"| {p['id']} | {p['name']} | 0x{p['size']:x} | {p['alignment']} | {p['capacity']} | {p['root_type'] or p['source_root_type'] or '-'} | {p['status']} |")
    path = ROOT / 'docs/mw19/pools.md'
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text('\n'.join(rows) + '\n', encoding='utf-8')
    print(json.dumps(dict(types=len(types), game_test=len(names), replay=len(pools),
                          pending=[p['name'] for p in pools if p['status'] == 'layout_pending'])))


if __name__ == '__main__':
    main()
