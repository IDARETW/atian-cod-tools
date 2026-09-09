"""Replay native fastfile regression tests. Only synthetic asset files are written.

Requires a built ACTS CLI and the owner's unmodified Replay executable. The
executable is mapped by ACTS; its game entrypoint is never called.
"""
import argparse
import csv
import io
import json
import struct
import subprocess
import sys
import zlib
from pathlib import Path
from fastfile_fixture import pack
from verify_linker import unpack
import parser_fixtures

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('exe', type=Path, help='Replay game executable')
    parser.add_argument('--acts', type=Path, default=ROOT / 'build/bin/Release/acts.exe')
    parser.add_argument('--out', type=Path, required=True, help='New, empty test directory')
    args = parser.parse_args()
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=False)
    base = [str(args.acts.resolve()), '--noUpdater']
    reader = base + ['fastfile', '-r', 'mw19replay', '-g', str(args.exe.resolve())]
    runs = []

    def run(label, command, success=True):
        result = subprocess.run([str(v) for v in command], cwd=ROOT, capture_output=True, timeout=180)
        (args.out / (label + '.log')).write_bytes(result.stdout + result.stderr)
        runs.append(dict(test=label, returncode=result.returncode))
        assert (result.returncode == 0) == success, (label, result.returncode, result.stdout[-2000:])

    def report(output, name):
        folder = output / 'mw19replay' / name
        manifest = json.loads((folder / 'manifest.json').read_text())
        rows = [json.loads(line) for line in (folder / 'assets.jsonl').read_text().splitlines()]
        assert len(rows) == manifest['tested']
        return manifest, rows

    def report_only(output):
        assert all(p.name in ('manifest.json', 'assets.jsonl') for p in output.rglob('*') if p.is_file())

    isolated = args.out / 'isolated'
    run('create-isolated', [sys.executable, ROOT / 'scripts/mw19/fastfile_fixture.py', isolated])
    fixtures = json.loads((isolated / 'fixtures.json').read_text())
    assert len(fixtures) == 111
    load_out = args.out / 'loaders'
    run('all-loaders', reader + ['--test', '--noAssetDump', '-o', load_out, isolated])
    for fixture in fixtures:
        m, rows = report(load_out, fixture['pool'])
        assert m['complete'] and m['success'] and m['loaded_assets'] == 1 and len(rows) == 1
    report_only(load_out)

    export_out = args.out / 'exporters'
    run('all-exporters', reader + ['--test', '-o', export_out, isolated], False)
    empty_payloads = {'image', 'soundbank', 'soundbanktransient'}
    for fixture in fixtures:
        m, rows = report(export_out, fixture['pool'])
        assert m['complete'] and m['failed'] == 0 and len(rows) == 1
        assert rows[0]['status'] == ('unavailable' if fixture['pool'] in empty_payloads else 'ok')
    report_only(export_out)

    # Resident stream-1 bytes must survive a later image reusing the buffer.
    for label in ('images', 'ddl', 'world_bounds', 'legacy_worlds'):
        fixture = args.out / (label + '.ff')
        fixture.write_bytes(getattr(parser_fixtures, label)())
        destination = args.out / ('parser-' + label)
        run('parser-' + label, reader + ['-o', destination, fixture])
        m, rows = report(destination, label)
        assert m['success'] and m['complete']
        folder = destination / 'mw19replay' / label
        if label == 'images':
            expected_pixels = [bytes.fromhex('ff0000ff00ff00ff0000ffffffffffff'), bytes(range(1, 17))]
            assert [(folder / r['file']).read_bytes()[148:] for r in rows] == expected_pixels
        elif label == 'ddl':
            fields = json.loads((folder / rows[0]['file']).read_text())['asset']['fields']
            members = fields['ddlDef']['values'][0]['structList']['values'][0]['members']['values']
            assert [m['name']['string'] for m in members] == ['first', 'second']
            assert [m['bitSize'] for m in members] == [32, 64]
            assert [m['arraySize'] for m in members] == [3, 4]
            assert members[0]['serializedByte']['bytes'] == 'a5'
        elif label == 'world_bounds':
            fields = json.loads((folder / rows[0]['file']).read_text())['asset']['fields']
            bounds = fields['surfaces']['surfaceBounds']
            assert bounds['stride'] == 56 and bounds['count'] == 2
            assert [v['bounds']['midPoint']['v'] for v in bounds['values']] == [[1, 2, 3], [11, 12, 13]]
            assert [v['bounds']['halfSize']['v'] for v in bounds['values']] == [[4, 5, 6], [14, 15, 16]]
            for i, value in enumerate(bounds['values']):
                assert value['serializedExtra']['bytes'] == bytes(range(i * 32, (i + 1) * 32)).hex()
        else:
            documents = {r['type']: json.loads((folder / r['file']).read_text())['asset']['fields'] for r in rows}
            assert [l['defName']['string'] for l in documents['com_map']['primaryLights']['values']] == ['point', 'spot']
            assert documents['scriptable']['models']['count'] == 2
            graphics = documents['gfx_map']
            assert graphics['frustumLights']['totalIndicesCount'] == 3
            assert graphics['frustumLights']['totalVerticesCount'] == 3
            assert graphics['lightViewFrustums']['values'][0]['indices']['bytes'] == '000102'
            serialized = json.loads((folder / next(r['file'] for r in rows if r['type'] == 'gfx_map')).read_text())['asset']['serialized_layout']
            assert serialized['xfile_version'] == 0xfda
            assert len(bytes.fromhex(serialized['root_bytes'])) == 17744
            assert serialized['light_meshes']['vertex_stride'] == 32
            assert len(bytes.fromhex(serialized['light_meshes']['headers'])) == 96
            assert len(bytes.fromhex(serialized['view_frustums']['headers'])) == 48
    # A final-revision file after an FDA file must use its original loader.
    mixed = args.out / 'mixed-revisions'; mixed.mkdir()
    (mixed / 'a.ff').write_bytes(parser_fixtures.legacy_worlds())
    (mixed / 'b.ff').write_bytes((isolated / 'gfx_map.ff').read_bytes())
    run('mixed-revision-loaders', reader + ['--test', '-o', args.out / 'mixed-output', mixed])

    inputs = args.out / 'populated'
    inputs.mkdir()
    run('create-populated', [sys.executable, ROOT / 'scripts/mw19/linker_fixture.py', inputs])
    linked = inputs / 'linked'
    run('link', base + ['fastfilelinker', '-o', linked, inputs / 'synthetic.zone'])
    ff = linked / 'zone/synthetic.ff'
    normal = args.out / 'roundtrip'
    run('export-populated', reader + ['-o', normal, ff])
    m, rows = report(normal, 'synthetic')
    assert m['success'] and len(rows) == 8
    expected = {r['pool']: r for r in json.loads((inputs / 'fixtures.json').read_text())}
    folder = normal / 'mw19replay/synthetic'
    for row in rows:
        data = (folder / row['file']).read_bytes()
        assert row['payload_bytes'] == len(data) and row['payload_crc32'] == zlib.crc32(data)
        source = expected[row['type']]
        if row['type'] in ('rawfile', 'ttf', 'luafile'):
            assert data == source['data'].encode()
        elif row['type'] == 'localize':
            assert json.loads(data) == dict(name=source['name'], value=source['value'])
        elif row['type'] == 'netconststrings':
            assert json.loads(data) == dict(stringType=2, sourceType=0, flags=0, strings=source['strings'])
        elif row['type'] == 'soundbanklist':
            assert json.loads(data) == source['hashes']
        elif row['type'] == 'stringtable':
            assert list(csv.reader(io.StringIO(data.decode()))) == source['rows']
        elif row['type'] == 'scriptfile':
            magic, compressed, decoded, code = struct.unpack_from('<4I', data)
            assert magic == 0x435347 and len(data) == 16 + compressed + code
            assert zlib.decompress(data[16:16+compressed]) == source['stack'].encode()
            assert decoded == len(source['stack']) and data[16+compressed:] == source['bytecode'].encode()
    sampled = args.out / 'sampled'
    run('test-populated', reader + ['--test', '-o', sampled, ff])
    _, checked = report(sampled, 'synthetic')
    assert [(r['type'], r['payload_crc32']) for r in checked] == [(r['type'], r['payload_crc32']) for r in rows]
    report_only(sampled)
    selected = args.out / 'selected'
    run('filters', reader + ['--test', '-a', 'rawfile,luafile', '-n', 'test.lua', '-o', selected, ff])
    _, checked = report(selected, 'synthetic')
    assert len(checked) == 1 and checked[0]['type'] == 'luafile'
    report_only(selected)

    # Add script strings, then verify all indices survive into their sidecar.
    body, blocks, _ = unpack(isolated / 'rawfile.ff')
    named = bytearray(body)
    struct.pack_into('<IIQ', named, 0, 3, 0, 1)
    named[32:32] = struct.pack('<qqq', 0, -2, -2) + b'alpha\0beta\0'
    special = args.out / 'strings.ff'
    special.write_bytes(pack(named, blocks))
    strings_out = args.out / 'strings'
    run('script-strings', reader + ['-o', strings_out, special])
    m, _ = report(strings_out, 'strings')
    assert m['success'] and m['script_strings'] == 3
    assert json.loads((strings_out / 'mw19replay/strings/script_strings.json').read_text()) == [None, 'alpha', 'beta']

    encoded_body = bytearray(body[:72]) + b'*/invalid:name\0'
    encoded_ff = args.out / 'encoded.ff'; encoded_ff.write_bytes(pack(encoded_body, blocks))
    encoded_out = args.out / 'encoded'
    run('encoded-name', reader + ['-o', encoded_out, encoded_ff])
    _, checked = report(encoded_out, 'encoded')
    assert checked[0]['filename_encoded'] and '/_encoded/' in checked[0]['file']
    assert checked[0]['name'] == '*/invalid:name'

    # Name-only references must not consume the one-per-type test quota.
    root = body[48:72]
    references = bytearray(struct.pack('<IIQIIQ', 0, 0, 0, 2, 0, 1))
    references += struct.pack('<IIqIIq', 51, 0, -2, 51, 0, -2)
    references += root + b',dependency\0' + root + b'actual\0'
    ref_ff = args.out / 'references.ff'
    ref_ff.write_bytes(pack(references, blocks))
    ref_out = args.out / 'references'
    run('references', reader + ['--test', '-o', ref_out, ref_ff])
    m, checked = report(ref_out, 'references')
    assert m['success'] and m['references'] == 1 and len(checked) == 1 and checked[0]['name'] == 'actual'
    report_only(ref_out)

    broken = args.out / 'malformed'
    broken.mkdir()
    variants = {}
    variants['truncated'] = body[:-1]
    variants['trailing'] = body + b'EXTRA'
    invalid = bytearray(body); struct.pack_into('<I', invalid, 16, 1000001)
    variants['asset-count'] = invalid
    invalid = bytearray(body); struct.pack_into('<I', invalid, 32, 117)
    variants['pool-id'] = invalid
    invalid = bytearray(body); struct.pack_into('<Q', invalid, 24, 0)
    variants['null-assets'] = invalid
    invalid = bytearray(body); struct.pack_into('<I', invalid, 0, 1)
    variants['null-strings'] = invalid
    invalid = bytearray(body); struct.pack_into('<q', invalid, 48, 0x700000fff)
    variants['string-offset'] = invalid
    invalid = bytearray(body); struct.pack_into('<q', invalid, 40, 0x700000fff)
    variants['asset-alias'] = invalid
    for name, content in variants.items():
        path = broken / (name + '.ff'); path.write_bytes(pack(content, blocks))
        out = args.out / ('reject-' + name)
        run('reject-' + name, reader + ['--test', '-o', out, path], False)
        m, _ = report(out, name)
        assert not m['success'] and not m['complete'] and m['error']
        report_only(out)
    for label, extra in [('zero-limit', ['--limit-per-pool', '0']), ('large-limit', ['--limit-per-pool', '33']),
                         ('dump-conflict', ['-d']), ('unknown-type', ['-a', 'missing'])]:
        run(label, reader + ['--test'] + extra + ['-o', args.out / label, ff], False)
    wrong_version = bytearray(pack(body, blocks)); struct.pack_into('<I', wrong_version, 12, 0xff9)
    wrong_file = args.out / 'wrong-version.ff'; wrong_file.write_bytes(wrong_version)
    run('wrong-version', reader + ['--test', '-o', args.out / 'wrong-version', wrong_file], False)
    # Revision metadata is retained; older unchanged Replay zones use the same
    # bounded stream path. Real installed-zone sampling complements this fixture.
    for version in (0xfcd, 0xfcf, 0xfd0, 0xfd1, 0xfd5, 0xfda, 0xfe1, 0xfe3, 0xfee, 0xff3, 0xff5):
        legacy = bytearray(pack(body, blocks)); struct.pack_into('<I', legacy, 12, version)
        legacy_file = args.out / f'legacy-{version:x}.ff'; legacy_file.write_bytes(legacy)
        legacy_out = args.out / f'legacy-{version:x}'
        run(f'legacy-{version:x}', reader + ['--test', '-o', legacy_out, legacy_file])
        legacy_report, _ = report(legacy_out, legacy_file.stem)
        assert legacy_report['success'] and legacy_report['xfile_version'] == version
    # A metadata-only .fp updates the revision and retains the base payload.
    old = bytearray(pack(body, blocks)); struct.pack_into('<I', old, 12, 0xff3)
    patched_file = args.out / 'header-only.ff'; patched_file.write_bytes(old)
    target = bytearray(old[:0x88]); struct.pack_into('<I', target, 12, 0xff7)
    patch_header = b'IWffd100' + struct.pack('<I', 6) + bytes(0x38 - 12)
    patch_file = patched_file.with_suffix('.fp')
    patch_file.write_bytes(patch_header + old[:0x88] + target)
    patched_out = args.out / 'header-only'
    run('header-only-patch', reader + ['--test', '-p', '-o', patched_out, patched_file])
    m, checked = report(patched_out, patched_file.stem)
    assert m['success'] and m['xfile_version'] == 0xff7 and len(checked) == 1
    assert m['input_xfile_version'] == 0xff3 and len(m['applied_patches']) == 1
    auto_out = args.out / 'auto-patch'
    run('auto-patch', reader + ['--test', '-o', auto_out, patched_file, legacy_file])
    m, _ = report(auto_out, patched_file.stem)
    assert m['success'] and m['xfile_version'] == 0xff7 and len(m['applied_patches']) == 1
    unpatched, _ = report(auto_out, legacy_file.stem)
    assert unpatched['xfile_version'] == 0xff5 and unpatched['applied_patches'] == []
    base_out = args.out / 'base-only'
    run('base-only', reader + ['--test', '--no-auto-patch', '-o', base_out, patched_file])
    m, _ = report(base_out, patched_file.stem)
    assert m['success'] and m['xfile_version'] == 0xff3 and m['applied_patches'] == []
    explicit_out = args.out / 'explicit-patch'
    run('explicit-patch', reader + ['--test', '--no-auto-patch', '-p', '-o', explicit_out, patched_file])
    m, _ = report(explicit_out, patched_file.stem)
    assert m['xfile_version'] == 0xff7 and len(m['applied_patches']) == 1
    # .fc must follow .fp and match the intermediate header, not the base.
    fc_file = patched_file.with_suffix('.fc')
    fc_file.write_bytes(patch_header + target + target)
    chain_out = args.out / 'auto-chain'
    run('auto-chain', reader + ['--test', '-o', chain_out, patched_file])
    m, _ = report(chain_out, patched_file.stem)
    assert [Path(p).suffix for p in m['applied_patches']] == ['.fp', '.fc']
    fc_file.write_bytes(patch_header + old[:0x88] + target)
    run('mismatched-auto-chain', reader + ['--test', '-o', args.out / 'bad-chain', patched_file], False)
    fc_file.unlink()  # Remove only this test's synthetic sidecar.
    previous = bytearray(old[:0x88]); previous[16] ^= 1
    patch_file.write_bytes(patch_header + previous + target)
    run('mismatched-auto-patch', reader + ['--test', '-o', args.out / 'bad-auto-patch', patched_file], False)
    patch_file.write_bytes(b'')
    run('empty-auto-patch', reader + ['--test', '-o', args.out / 'empty-auto-patch', patched_file], False)
    # Header edits that imply different payload bytes still require delta data.
    target[32] ^= 1
    patch_file.write_bytes(patch_header + old[:0x88] + target)
    run('missing-patch-delta', reader + ['--test', '-p', '-o', args.out / 'missing-delta', patched_file], False)
    # Already loaded roots remain exportable when a subsequent record is bad.
    partial = bytearray(references.replace(b',dependency\0', b'kept_asset\0')); struct.pack_into('<I', partial, 48, 117)
    partial_file = args.out / 'partial-load.ff'; partial_file.write_bytes(pack(partial, blocks))
    partial_out = args.out / 'partial-load'
    run('partial-load-export', reader + ['-o', partial_out, partial_file], False)
    m, checked = report(partial_out, partial_file.stem)
    assert not m['complete'] and m['partial_load'] and m['loaded_assets'] == 1 and len(checked) == 1
    run('wrong-executable', base + ['fastfile', '-r', 'mw19replay', '-g', args.acts.resolve(), '--test',
                                  '-o', args.out / 'wrong-executable', ff], False)
    run('missing-handler', base + ['fastfile', '--test', '-o', args.out / 'missing-handler', ff], False)
    huge_blocks = list(blocks); huge_blocks[1] = 2 * 1024**3 + 1
    huge_file = args.out / 'huge-reservation.ff'; huge_file.write_bytes(pack(body, huge_blocks))
    run('huge-reservation', reader + ['--test', '-o', args.out / 'huge-reservation', huge_file], False)
    (args.out / 'tests.json').write_text(json.dumps(dict(success=True, runs=runs), indent=2) + '\n')
    print('111 loaders, 108 empty-root exporters, 3 explicit empty payloads, eight populated round trips, '
          'resident image retention, 64-byte DDL members, populated FDA lighting and scriptables, '
          'legacy revisions, header-only patches, partial-load recovery, filters, script strings, '
          'reference quotas and malformed input checks passed. Synthetic assets only.')


if __name__ == '__main__':
    main()
