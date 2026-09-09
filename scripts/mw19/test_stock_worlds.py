"""Validate the four selected Replay worlds without starting the game.

Default output is capability reports only. --write-worlds additionally writes
the four GfxWorld JSON records, never an unrestricted asset dump.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[2]
NAMES = ('mp_frontend3', 'mp_rust', 'mp_hideout', 'mp_shipment')


def validate_bounds(document):
    surfaces = document['asset']['fields']['surfaces']
    bounds = surfaces['surfaceBounds']
    assert bounds['stride'] == 56 and bounds['count'] == surfaces['count']
    if 'bytes' in bounds:
        data = bytes.fromhex(bounds['bytes'])
        assert len(data) == bounds['count'] * 56
        records = (struct.unpack_from('<6f', data, offset) for offset in range(0, len(data), 56))
    else:
        assert len(bounds['values']) == bounds['count']
        for value in bounds['values']:
            assert len(bytes.fromhex(value['serializedExtra']['bytes'])) == 32
        records = (value['bounds']['midPoint']['v'] + value['bounds']['halfSize']['v'] for value in bounds['values'])
    for index, values in enumerate(records):
        assert all(math.isfinite(v) for v in values), ('non-finite bounds', index)
        assert all(v >= 0 for v in values[3:]), ('negative half-size', index, values)
    return bounds['count']


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--game', type=Path, required=True)
    p.add_argument('--zone', type=Path, required=True)
    p.add_argument('--oodle', type=Path, required=True)
    p.add_argument('--acts', type=Path, default=ROOT / 'build/bin/Release/acts.exe')
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--write-worlds', action='store_true')
    args = p.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    files = [args.zone / (name + '.ff') for name in NAMES]
    assert all(path.is_file() for path in files)
    command = [args.acts.resolve(), '--noUpdater', 'fastfile', '-r', 'mw19replay',
               '-g', args.game.resolve(), '--oodle', args.oodle.resolve()]
    # Deliberately omit -p/--fc: this regression exercises automatic discovery.
    def run(label, options):
        with (args.out / (label + '.log')).open('wb') as log:
            result = subprocess.run([str(v) for v in command + options + files], cwd=ROOT,
                                    stdout=log, stderr=subprocess.STDOUT, timeout=600)
        assert result.returncode == 0, (label, 'see log', result.returncode)
    run('capabilities', ['--test', '-a', 'gfx_map,gfx_map_trzone,com_map', '-o', args.out / 'reports'])
    summary = []
    for name, source in zip(NAMES, files):
        report = json.loads((args.out / 'reports/mw19replay' / name / 'manifest.json').read_text())
        with source.open('rb') as stream:
            input_version = struct.unpack('<4I', stream.read(16))[3]
        assert report['complete'] and report['success'] and report['tested'] == 3
        assert report['failed'] == report['unavailable'] == 0 and report['xfile_version'] == 0xff7
        assert report['input_xfile_version'] == input_version
        expected = [str(source.with_suffix(ext)) for ext in ('.fp', '.fc') if source.with_suffix(ext).exists()]
        assert [Path(v).resolve() for v in report['applied_patches']] == [Path(v).resolve() for v in expected]
        summary.append({key: report[key] for key in ('fastfile', 'loaded_assets', 'serialized_bytes_read',
                        'input_xfile_version', 'xfile_version', 'applied_patches', 'tested', 'failed', 'unavailable')})
    assert all(path.name in ('manifest.json', 'assets.jsonl') for path in (args.out / 'reports').rglob('*') if path.is_file())
    if args.write_worlds:
        run('world-json', ['-a', 'gfx_map', '-o', args.out / 'worlds'])
        for item, name in zip(summary, NAMES):
            folder = args.out / 'worlds/mw19replay' / name
            report = json.loads((folder / 'manifest.json').read_text())
            assert report['complete'] and report['success']
            rows = [json.loads(line) for line in (folder / 'assets.jsonl').read_text().splitlines()]
            worlds = [row for row in rows if row['type'] == 'gfx_map' and row.get('file')]
            assert len(worlds) == 1
            path = folder / worlds[0]['file']
            item['verified_surface_bounds'] = validate_bounds(json.loads(path.read_text(encoding='utf-8')))
            item['world_json'] = str(path)
            item['world_sha256'] = hashlib.sha256(path.read_bytes()).hexdigest()
    (args.out / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps(summary, indent=2))


if __name__ == '__main__':
    main()
