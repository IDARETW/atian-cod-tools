"""Inventory potential traversal gaps using only the shipped schema.

This does not read a process or any asset data. It includes declared descendants
of unresolved pointers, so counts are potential gaps, not observed failures.
Asset references are separate export units. Runtime descendants are kept in
the inventory until their non-serialized role is explicitly classified.
"""
import argparse
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def audit(schema, profile_id='replay-1.20'):
    profile = schema['profiles'][profile_id]
    overrides = profile.get('type_overrides', {})
    types = schema['types']
    roots = {p['root_type'] for p in profile['pools'] if p['root_type']}
    pointers = schema.get('pointer_rules', {})
    unions = schema.get('union_rules', {})

    def type_of(name):
        return types[overrides.get(name, name)]

    def contains_pointers(name, seen=None):
        seen = set() if seen is None else seen
        if name in seen:
            return False
        seen.add(name)
        value = type_of(name)
        if value['kind'] == 'pointer':
            return True
        if value['kind'] == 'array':
            return contains_pointers(value['element'], seen)
        return any(contains_pointers(m['type'], seen) for m in value.get('members', []))

    results = []
    for pool in profile['pools']:
        seen, gaps = set(), {}

        def member(owner, path, name, rule=None):
            value = type_of(name)
            kind = value['kind']
            if kind == 'pointer':
                if rule and 'runtime' in rule:
                    return
                target = value['target']
                if target in roots and (rule is None or rule.get('count') == 1):
                    return
                if rule is None:
                    if target in ('char', 'const char') or target.startswith('ID3D') or '__fastcall(' in target:
                        return
                    key = (owner, path, 'pointer_extent_required')
                    gaps[key] = dict(owner=owner, member=path, target=target, reason=key[2])
                if type_of(target)['kind'] in ('pointer', 'array'):
                    member(owner, path + '->[]', target, rule.get('pointee') if rule else None)
                else:
                    walk(target)
            elif kind == 'array':
                elements = rule.get('elements') if rule else None
                for index in range(value['count']):
                    member(owner, f'{path}[{index}]', value['element'], elements[index] if elements else None)
            else:
                walk(name)

        def walk(name):
            if name in seen:
                return
            seen.add(name)
            value = type_of(name)
            if value['kind'] == 'union' and (contains_pointers(name) or name in unions):
                if name not in unions:
                    gaps[(name, '', 'selector_required')] = dict(owner=name, member='', reason='selector_required')
                    return
                members = {m['name']: m for m in value['members']}
                for choice in unions[name]['cases']:
                    m = members[choice['member']]
                    mt = type_of(m['type'])
                    rule = None
                    if 'runtime' in choice:
                        rule = dict(runtime=choice['runtime'])
                    elif mt['kind'] == 'pointer' and not choice.get('string'):
                        rule = dict(count=choice.get('count', 1))
                    elif 'element_count' in choice:
                        rule = dict(elements=[dict(count=choice['element_count'])] * mt['count'])
                    member(name, m['name'], m['type'], rule)
            elif value['kind'] in ('struct', 'union'):
                for m in value['members']:
                    member(name, m['name'], m['type'], pointers.get(name, {}).get(m['name']))
            elif value['kind'] == 'array':
                member(name, '', name)
            elif value['kind'] == 'pointer':
                member(name, '', name)

        if pool['root_type']:
            walk(pool['root_type'])
        results.append(dict(id=pool['id'], name=pool['name'], root_type=pool['root_type'],
                            layout_evidence=pool['status'], potential_gaps=[gaps[k] for k in sorted(gaps)]))
    unique = {(g['owner'], g['member'], g['reason']) for p in results for g in p['potential_gaps']}
    return dict(profile=profile_id, scope='static declared type graph; no process or asset reads',
                caveats=['Runtime descendants may not represent serialized content.',
                         'Rule presence does not prove native layout or every union selector.',
                         'Payload codecs and external residency are assessed separately.'],
                pool_count=len(results), unique_potential_gaps=len(unique),
                pools_with_potential_gaps=sum(bool(p['potential_gaps']) for p in results), pools=results)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--schema', type=Path, default=ROOT / 'config/data/mw19/schema.json')
    parser.add_argument('--profile', default='replay-1.20')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    report = audit(json.loads(args.schema.read_text(encoding='utf-8')), args.profile)
    if args.output:
        if args.output.resolve() == args.schema.resolve() or (args.output.exists() and args.output.samefile(args.schema)):
            raise ValueError('Report output must not overwrite the input schema')
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({k: v for k, v in report.items() if k != 'pools'}))


if __name__ == '__main__':
    main()
