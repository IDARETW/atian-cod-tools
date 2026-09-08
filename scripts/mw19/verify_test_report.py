"""Check report-only exports against independently specified fixture bytes."""
import json
import struct
import sys
import zlib
from pathlib import Path
from verify_sound_report import fixture_bank, fixture_flac


def rgba_dds(array=False):
    # Independently specified Microsoft DDS/DX10 header for the two fixtures.
    fields = [0] * 37
    fields[:8] = [0x20534444, 124, 0x2100f if array else 0x100f, 1, 2 if array else 1, 8 if array else 4, 0, 2 if array else 1]
    fields[19:22] = [32, 4, 0x30315844]
    fields[27] = 0x401008 if array else 0x1000
    fields[32:37] = [28, 3, 0, 2 if array else 1, 0]
    return struct.pack('<37I', *fields) + (b'AAAAAAAACCCCBBBBBBBBDDDD' if array else b'RGBA')


def verify(directory, negative=False):
    directory = Path(directory)
    manifest = json.loads((directory / 'manifest.json').read_text())
    assert manifest['test_only'] and manifest['complete']
    assert {p.name for p in directory.iterdir()} == {'manifest.json', 'progress.json', 'assets.jsonl'}
    if negative:
        assert not manifest['success'] and manifest['failures'] == manifest['partials'] == 1
        assert len(manifest['assets']) == 4
        assets = {a['name']: a for a in manifest['assets']}
        missing = assets['fixture/image/2']
        assert missing['status'] == 'partial' and missing['payload_status'] == 'unavailable'
        assert missing['payload_reason'] == 'image_uploaded' and 'payload_bytes' not in missing
        assert missing['external_payloads'] == 1 and missing['field_read_errors'] == 0
        assert missing['field_issues'][0]['reason'] == 'resident_payload_unavailable'
        bad = assets['fixture/image/3']
        assert bad['status'] == bad['field_status'] == 'failed' and bad['payload_status'] == 'ok'
        assert bad['field_read_errors'] == 1 and bad['field_issue_count'] == 1
        assert bad['field_issues'][0]['path'] == 'fields/packedAtlasData'
        assert 'Unreadable memory' in bad['field_issues'][0]['reason']
        assert bad['payload_bytes'] == len(rgba_dds()) and bad['payload_crc32'] == zlib.crc32(rgba_dds())
        assert not any(k.endswith('_file') for a in manifest['assets'] for k in a)
        print('Missing image pixels reported partial; malformed auxiliary field reported with its path; DDS payload still checked. No asset files written.')
        return
    assert manifest['success'] and manifest['failures'] == manifest['partials'] == 0
    assert len(manifest['assets']) == 22
    def js(value): return (json.dumps(value, indent=2) + '\n').encode()
    expected = {
        'rawfile': b'fixture payload\n',
        'luafile': b'DATA', 'ttf': b'DATA',
        'scriptfile': struct.pack('<4I', 0x435347, 3, 5, 3) + b'bufgsc',
        'stringtable': b'"first","quoted ""cell"""\n',
        'localize': js(dict(name='fixture/localize/0', value='Fixture translation')),
        'netconststrings': js(dict(stringType=0, sourceType=0, flags=0, strings=['first', 'second'])),
        'soundbanklist': js([0x12345678, 0x89abcdef]),
        'streamkey': b'\x00\x01\xff\x80MESH\x00', 'xmodelsurfs': b'\x00\x01\xff\x80MESH\x00',
        'soundbank': fixture_bank(), 'soundbanktransient': fixture_bank(),
    }
    for asset in manifest['assets']:
        value = expected.get(asset['pool'], b'DXBCfixture')
        if asset['pool'] == 'image': value = rgba_dds(asset['name'].endswith('/1'))
        assert asset['status'] == asset['payload_status'] == 'ok', asset
        assert asset['payload_bytes'] == len(value), asset
        assert asset['payload_crc32'] == zlib.crc32(value), asset
        assert not any(key.endswith('_file') for key in asset), asset
        if asset['pool'] == 'ttf':
            assert asset['runtime_references'] == 1 and asset['field_read_errors'] == asset['unresolved_pointers'] == 0
        if asset['pool'] in ('soundbank', 'soundbanktransient'):
            assert asset['audio_status'] == 'ok' and len(asset['audio_samples']) == asset['audio_total_samples'] == 1
            sound = asset['audio_samples'][0]
            assert sound['status'] == 'ok' and sound['bytes'] == len(fixture_flac()) and sound['crc32'] == zlib.crc32(fixture_flac())
            assert 'file' not in sound
    print('22 sampled assets: every payload length and CRC matches, including DDS, stream/mesh and SAB/FLAC; no asset files written.')


if __name__ == '__main__': verify(sys.argv[1], '--negative' in sys.argv[2:])
