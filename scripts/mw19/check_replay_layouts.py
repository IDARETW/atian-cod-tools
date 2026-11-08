"""Check the generated MW2019 Replay 1.20 layouts against x64-zt's loader-verified corrections.

Builds the schema in memory (same steps as generate_schema.py, nothing written), then:
  1. every record in x64-zt tools/iw8_layouts.txt must match the final ACTS type it overrides:
     same size and the same member offsets (names may differ only where ACTS keeps a field unnamed);
  2. every struct in the schema must be self-consistent: members inside the struct, no overlaps
     (bitfields that share storage are allowed), sizes taken from the final member types.

Usage: python -I scripts/mw19/check_replay_layouts.py [--x64zt E:/mw124/x64-zt] [--json report.json]
Exit code 0 only when both checks pass.
"""
import argparse
import importlib.util
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))


def build_schema():
    import generate_schema as g
    captured = {}

    def capture(path, value):
        captured['schema'] = value

    original_write, original_header = g.write, g.replay_image.write_header
    g.write = capture
    g.replay_image.write_header = lambda p: None
    pools_md = g.ROOT / 'docs/mw19/pools.md'
    before = pools_md.read_bytes() if pools_md.exists() else None
    try:
        g.main()
    finally:
        g.write, g.replay_image.write_header = original_write, original_header
        if before is not None and pools_md.read_bytes() != before:
            pools_md.write_bytes(before)  # dry run: keep the committed coverage table untouched
    return captured['schema']


def load_x64zt(root):
    spec = importlib.util.spec_from_file_location('iw8_layouts', root / 'tools/iw8_layouts.py')
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod.parse(str(root / 'tools/iw8_layouts.txt'))


def resolve(types, overrides, name):
    name = name.strip()
    if name in types:
        return name
    return overrides.get(name, name)


def member_bytes(types, overrides, m):
    if 'size_bits' in m and m['size_bits'] is not None:
        return m['size_bits'] / 8
    t = types.get(resolve(types, overrides, m['type']))
    return t['size'] if t else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--x64zt', default='E:/mw124/x64-zt')
    ap.add_argument('--json')
    args = ap.parse_args()
    schema = build_schema()
    types = schema['types']
    profile = schema['profiles']['replay-1.20']
    overrides = profile.get('type_overrides', {})
    records, aliases = load_x64zt(Path(args.x64zt))
    report = dict(records=[], structs=[], summary={})

    # 1. x64-zt records vs final ACTS types.
    bad_records = 0
    for name, rec in records.items():
        base = name.split('::', 1)[1] if name.startswith('Replay::') else name
        target = name if name in types else overrides.get(base)
        entry = dict(record=name, acts_type=target, problems=[])
        t = types.get(target) if target else None
        if not t:
            entry['problems'].append('no ACTS type for this record')
        else:
            if 'size' in rec and t.get('size') != rec['size']:
                entry['problems'].append(f"size {t.get('size')} != x64-zt {rec['size']}")
            if rec.get('kind') == 'patch' and t.get('kind') == 'struct':
                # A patch places listed members; compare by offset and type (ACTS may keep the
                # game-test member name, e.g. weaponOffsetPatternScaleInfo vs x64-zt weaponOffsetPatternScales).
                at = {m['offset_bits'] // 8: m for m in t.get('members', [])}
                for off, mname, mtype in rec['members']:
                    m = at.get(off)
                    if not m or m['type'].replace(' ', '') != mtype.replace(' ', ''):
                        entry['problems'].append(f'{mname} ({mtype}) at +{off}: ACTS has {m and (m["name"], m["type"])}')
            elif t.get('kind') in ('struct', 'union') and rec['members']:
                acts_offsets = sorted({m['offset_bits'] // 8 for m in t.get('members', [])})
                x_offsets = sorted({m[0] for m in rec['members']})
                missing = [o for o in x_offsets if o not in acts_offsets]
                extra = [o for o in acts_offsets if o not in x_offsets]
                if missing:
                    entry['problems'].append(f'x64-zt member offsets absent in ACTS: {missing[:12]}')
                if extra and t.get('kind') == 'struct':
                    entry['problems'].append(f'ACTS member offsets absent in x64-zt: {extra[:12]}')
        bad_records += bool(entry['problems'])
        report['records'].append(entry)

    # 2. Self-consistency of every struct.
    bad_structs = 0
    for name, t in types.items():
        if t.get('kind') != 'struct':
            continue
        size_bits = t['size'] * 8
        problems = []
        spans = []
        for m in t.get('members', []):
            start = m['offset_bits']
            nbytes = member_bytes(types, overrides, m)
            if nbytes is None:
                problems.append(f"{m['name']}: unknown type {m['type']}")
                continue
            end = start + int(nbytes * 8)
            if end > size_bits:
                problems.append(f"{m['name']}: ends at byte {end / 8:g} past size {t['size']}")
            spans.append((start, end, m['name']))
        spans.sort()
        for (s0, e0, n0), (s1, e1, n1) in zip(spans, spans[1:]):
            if s1 < e0 and not (e0 - s0 < 8 * 8 and s1 // 64 == s0 // 64 and (e1 - s1) < 64):  # allow bitfields in one word
                problems.append(f'{n1} at byte {s1 / 8:g} overlaps {n0} ending at byte {e0 / 8:g}')
        if problems:
            bad_structs += 1
            report['structs'].append(dict(type=name, problems=problems[:6]))

    # 3. Pointer rules must bind to the final (overridden) types, as mw19_schema.cpp validates at load:
    #    the member must exist and be a pointer or array; count paths must name existing members.
    bad_rules = []

    def rtype(name):
        # Load-time validation (mw19_schema.cpp) resolves rule owners and member types by their
        # literal names, without profile overrides; Replay-keyed rules name the Replay type directly.
        return types.get(name)

    def check_paths(expr, where):
        if isinstance(expr, dict):
            if 'owner' in expr and 'path' in expr:
                # Count paths are evaluated during traversal (Inspector::Type applies overrides).
                t = types.get(overrides.get(expr['owner'], expr['owner']))
                cur = t
                for step in expr['path']:
                    if isinstance(step, int):
                        if not cur or cur.get('kind') != 'array':
                            bad_rules.append(f'{where}: index into non-array'); return
                        cur = types.get(overrides.get(cur['element'], cur['element']))
                        continue
                    if not cur or cur.get('kind') not in ('struct', 'union'):
                        bad_rules.append(f'{where}: path {expr["owner"]}.{".".join(map(str, expr["path"]))} not resolvable'); return
                    m = next((m for m in cur['members'] if m['name'] == step), None)
                    if not m:
                        bad_rules.append(f'{where}: count path member {expr["owner"]}.{step} missing'); return
                    cur = types.get(overrides.get(m['type'], m['type']))
            for v in expr.values():
                check_paths(v, where)
        elif isinstance(expr, list):
            for v in expr:
                check_paths(v, where)

    for owner, rules in schema.get('pointer_rules', {}).items():
        t = rtype(owner)
        if not t or t.get('kind') not in ('struct', 'union'):
            bad_rules.append(f'{owner}: rule owner is not a struct')
            continue
        for field, rule in rules.items():
            m = next((m for m in t['members'] if m['name'] == field), None)
            if not m:
                bad_rules.append(f'{owner}.{field}: member missing in {overrides.get(owner, owner)}')
                continue
            mt = rtype(m['type'])
            if not mt or mt.get('kind') not in ('pointer', 'array'):
                bad_rules.append(f'{owner}.{field}: not a pointer/array in {overrides.get(owner, owner)}')
            # Count paths only matter for rules traversal can use: the traversed (overridden) type
            # must have this member (Value() looks rules up by the traversed type's member names).
            traversed = types.get(overrides.get(owner, owner)) or t
            if any(x['name'] == field for x in traversed.get('members', [])):
                check_paths(rule, f'{owner}.{field}')
    for b in bad_rules[:80]:
        print('RULE', b)

    report['rules'] = bad_rules
    report['summary'] = dict(pointer_rule_problems=len(bad_rules), x64zt_records=len(records), records_mismatched=bad_records,
                             structs=sum(1 for t in types.values() if t.get('kind') == 'struct'),
                             structs_inconsistent=bad_structs, type_overrides=len(overrides))
    for e in report['records']:
        if e['problems']:
            print('RECORD', e['record'], '->', e['acts_type'], '|', '; '.join(e['problems']))
    for s in report['structs'][:60]:
        print('STRUCT', s['type'], '|', '; '.join(s['problems']))
    print(json.dumps(report['summary']))
    if args.json:
        Path(args.json).write_text(json.dumps(report, indent=1), encoding='utf-8')
    return 0 if not bad_records and not bad_structs and not bad_rules else 1


if __name__ == '__main__':
    sys.exit(main())
