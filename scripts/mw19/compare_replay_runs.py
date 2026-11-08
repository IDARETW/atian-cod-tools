"""Compare two ACTS mw19replay runs of the same zones (e.g. before/after a schema change).

Each run directory holds <zone>/mw19replay/<zone>/{manifest.json,assets.jsonl}. The report lists, per zone
and asset type: counts by status, and the sums of read_errors, unresolved_pointers, unresolved_unions,
external_payloads and field_nodes. A regression is any type whose ok-count drops or whose error counters rise.

Usage: python -I scripts/mw19/compare_replay_runs.py <before_dir> <after_dir> [--json out.json]
"""
import argparse
import collections
import json
import sys
from pathlib import Path

COUNTERS = ('read_errors', 'unresolved_pointers', 'unresolved_unions', 'external_payloads')


def load(run):
    zones = {}
    for manifest in Path(run).glob('*/mw19replay/*/manifest.json'):
        zone = manifest.parent.name
        m = json.loads(manifest.read_text(encoding='utf-8'))
        stats = collections.defaultdict(lambda: collections.Counter())
        journal = manifest.parent / m.get('journal', 'assets.jsonl')
        if journal.exists():
            for line in journal.read_text(encoding='utf-8').splitlines():
                if not line.strip():
                    continue
                r = json.loads(line)
                s = stats[r['type']]
                s['status:' + str(r.get('status'))] += 1
                for c in COUNTERS + ('field_nodes',):
                    s[c] += int(r.get(c) or 0)
        zones[zone] = dict(complete=m.get('complete'), success=m.get('success'), tested=m.get('tested'),
                           failed=m.get('failed'), unavailable=m.get('unavailable'), loaded=m.get('loaded_assets'),
                           types={t: dict(c) for t, c in stats.items()})
    return zones


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('before')
    ap.add_argument('after')
    ap.add_argument('--json')
    a = ap.parse_args()
    before, after = load(a.before), load(a.after)
    regressions, improvements, rows = [], [], []
    for zone in sorted(set(before) | set(after)):
        b, f = before.get(zone), after.get(zone)
        if not b or not f:
            regressions.append(f'{zone}: present in only one run')
            continue
        rows.append(f"{zone}: loaded {b['loaded']} -> {f['loaded']}, tested {b['tested']} -> {f['tested']}, "
                    f"failed {b['failed']} -> {f['failed']}, unavailable {b['unavailable']} -> {f['unavailable']}, complete {f['complete']}")
        if f['loaded'] != b['loaded'] or not f['complete']:
            regressions.append(f'{zone}: loaded/complete changed')
        for t in sorted(set(b['types']) | set(f['types'])):
            bt, ft = collections.Counter(b['types'].get(t, {})), collections.Counter(f['types'].get(t, {}))
            if ft['status:ok'] < bt['status:ok']:
                regressions.append(f"{zone}/{t}: ok {bt['status:ok']} -> {ft['status:ok']}")
            elif ft['status:ok'] > bt['status:ok']:
                improvements.append(f"{zone}/{t}: ok {bt['status:ok']} -> {ft['status:ok']}")
            for c in COUNTERS:
                if ft[c] > bt[c]:
                    regressions.append(f'{zone}/{t}: {c} {bt[c]} -> {ft[c]}')
                elif ft[c] < bt[c]:
                    improvements.append(f'{zone}/{t}: {c} {bt[c]} -> {ft[c]}')
            if ft['field_nodes'] != bt['field_nodes']:
                rows.append(f"  {t}: field_nodes {bt['field_nodes']} -> {ft['field_nodes']}")
    print('\n'.join(rows))
    print(f'\n{len(regressions)} regressions')
    for r in regressions:
        print('  REGRESSION', r)
    print(f'{len(improvements)} improvements')
    for r in improvements:
        print('  improved', r)
    if a.json:
        Path(a.json).write_text(json.dumps(dict(before=before, after=after, regressions=regressions,
                                                improvements=improvements), indent=1), encoding='utf-8')
    return 1 if regressions else 0


if __name__ == '__main__':
    sys.exit(main())
