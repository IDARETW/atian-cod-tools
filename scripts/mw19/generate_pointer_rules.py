"""Extract explicit pointer extents from the game-test serialization routines.

Only expressions reducible to constants and named scalar members are accepted.
Conflicting observations are reported instead of choosing an arbitrary extent.
This is source evidence; Replay layout validation is a separate step.
"""
import ast
import json
import re
from pathlib import Path
import reference_rules

ROOT = Path(__file__).resolve().parents[2]
CAST = re.compile(r'\((?:(?:const|unsigned|signed|struct)\s+)*(?:[A-Za-z_]\w*)(?:\s*\*)*\s*\)')
SUFFIX = re.compile(r'\b(0x[\da-fA-F]+|\d+)(?:ui64|i64|u|LL|L)\b')


def simplify(text, aliases):
    text = SUFFIX.sub(r'\1', text.strip())
    text = CAST.sub('', text)
    # Aliases hold expanded expressions from the point of assignment.
    text = re.sub(r'(?<![.>\w])\b\w+\b', lambda m: aliases.get(m[0], m[0]), text)
    return text.strip()


def expression(text, types):
    fields = []
    def field(match):
        owner = match[1]
        path = [int(v[1:-1]) if v.startswith('[') else v for v in re.findall(r'\w+|\[\d+\]', match[2])]
        if owner not in types: raise ValueError(owner)
        at = types[owner]
        for part in path:
            if isinstance(part, int):
                if at['kind'] != 'array' or part >= at['count']: raise ValueError('Array index')
                at = types[at['element']]
            else:
                member = next(m for m in at.get('members', []) if m['name'] == part)
                at = types[member['type']]
        if at['kind'] not in ('scalar', 'enum') or not 0 < at['size'] <= 8:
            raise ValueError('Non-scalar count')
        fields.append(dict(owner=owner, path=path))
        return f'F{len(fields)-1}'
    text = re.sub(r'var(\w+)->(\w+(?:\[\d+\])?(?:(?:->|\.)\w+(?:\[\d+\])?)*)', field, text)
    operations = {ast.Add: 'add', ast.Sub: 'sub', ast.Mult: 'mul', ast.Div: 'div', ast.FloorDiv: 'div',
                  ast.RShift: 'shr', ast.LShift: 'shl', ast.BitAnd: 'and', ast.BitOr: 'or'}
    comparisons = {ast.Eq: 'eq', ast.NotEq: 'ne', ast.Gt: 'gt', ast.GtE: 'ge'}
    def node(value):
        if isinstance(value, ast.Constant) and isinstance(value.value, int) and value.value >= 0: return value.value
        if isinstance(value, ast.Name) and re.fullmatch(r'F\d+', value.id): return fields[int(value.id[1:])]
        if isinstance(value, ast.BinOp) and type(value.op) in operations:
            return dict(op=operations[type(value.op)], args=[node(value.left), node(value.right)])
        if isinstance(value, ast.Compare) and len(value.ops) == 1 and type(value.ops[0]) in comparisons:
            return dict(op=comparisons[type(value.ops[0])], args=[node(value.left), node(value.comparators[0])])
        raise ValueError('Unsupported count expression: ' + text)
    return node(ast.parse(text, mode='eval').body)


def calls(code):
    for match in re.finditer(r'\b(Load_\w+)\(', code):
        level, end, start, args = 1, match.end(), match.end(), []
        while level:
            char = code[end]
            if char == '(': level += 1
            if char == ')': level -= 1
            if (char == ',' and level == 1) or not level:
                args.append(code[start:end].strip()); start = end + 1
            end += 1
        yield match.start(), 'call', (match[1], args)


def array_strides(sources):
    result = {}
    for name, source in sources.items():
        if not name.endswith('Array'): continue
        for _, _, (loader, args) in calls(source['code']):
            if loader != 'Load_Stream' or args[0] != 'streamStart' or len(args) != 3: continue
            text = SUFFIX.sub(r'\1', args[2])
            def size(count):
                def value(node):
                    if isinstance(node, ast.Constant) and isinstance(node.value, int): return node.value
                    if isinstance(node, ast.Name) and node.id == 'count': return count
                    if isinstance(node, ast.BinOp) and isinstance(node.op, ast.Mult): return value(node.left) * value(node.right)
                    if isinstance(node, ast.BinOp) and isinstance(node.op, ast.LShift): return value(node.left) << value(node.right)
                    raise ValueError('Not a constant array stride')
                return value(ast.parse(text, mode='eval').body)
            try:
                stride = size(1)
                if 0 < stride <= 1024 * 1024 and size(0) == 0 and size(2) == 2 * stride:
                    result[name[:-5]] = stride
            except (ValueError, SyntaxError): pass
            break
    return result


def generate(schema, sources):
    types = schema['types']
    rules, rejected = {}, []
    source_aliases = {'UnsignedShort': 'unsigned __int16', 'ushort': 'unsigned __int16',
                      'UnsignedInt': 'unsigned int', 'UnsignedInt64': 'unsigned __int64',
                      'ConstChar': 'const char', 'ConstByte': 'const unsigned __int8',
                      'byte': 'unsigned __int8', 'char': 'char', 'XString': 'const char *',
                      'MaterialHandle': 'Material *', 'uint': 'unsigned int', 'int': 'int',
                      'short': '__int16', 'float': 'float', 'vec3_t': 'vec3_t'}
    # Decompiled allocators often return byte* even for ushort/int storage.
    # Use their array routine's actual Load_Stream extent, not that return type.
    strides = array_strides(sources)
    for name, type in types.items():
        source_key = name if name in sources else name + 'Array'
        if type['kind'] != 'struct' or source_key not in sources: continue
        source = sources[source_key]
        code = source['code']
        members = {}
        def pointer_members(label, member_type, path):
            value = types[member_type]
            if value['kind'] == 'pointer':
                members[label] = dict(type=member_type, path=path)
            elif value['kind'] == 'array':
                for index in range(value['count']):
                    pointer_members(f'{label}[{index}]', value['element'], path + [index])
        for member in type['members']:
            pointer_members(member['name'], member['type'], [member['name']])
        if not members: continue
        aliases, candidate, found = {}, None, {}
        events = list(calls(code))
        for match in re.finditer(r'\b(\w+(?:->\w+)?(?:\[\d+\])*)\s*=\s*([^;]+);', code):
            if match[2].startswith('='): continue
            events.append((match.start(), 'assignment', (match[1], match[2])))
        # A loop accumulator is no longer its initial constant after mutation.
        # Unsupported updates must invalidate that alias, never produce a
        # plausible zero-length extent from the initial assignment.
        for match in re.finditer(r'(?<![\w.>])(\w+)\s*(?:\+\+|--|[+*/%&|^\-]=|<<=|>>=)', code):
            events.append((match.start(), 'mutation', match[1]))
        for match in re.finditer(r'(?:\+\+|--)\s*(\w+)\b', code):
            events.append((match.start(), 'mutation', match[1]))
        for position, kind, event in sorted(events):
            if kind == 'mutation':
                aliases.pop(event, None)
                continue
            if kind == 'assignment':
                left, right = event
                left = simplify(left, aliases)
                right = simplify(right, aliases)
                m = re.fullmatch(r'var' + re.escape(name) + r'->(\w+(?:\[\d+\])*)', left)
                address = re.fullmatch(r'&var' + re.escape(name) + r'->(\w+(?:\[\d+\])*)', right)
                if m and m[1] in members:
                    candidate = m[1]
                elif address and address[1] in members:
                    candidate = address[1]
                if '->' not in event[0]:
                    # Preserve canonical varType roots; a varElement binding is
                    # transient serialization state, not a new owner object.
                    if not event[0].startswith('var'):
                        aliases[event[0]] = right
                continue
            loader, args = event
            if candidate is None or not args: continue
            if loader == 'Load_XString':
                # Consumed as a string, never carry its member forward to an
                # unrelated array loader later in the containing routine.
                candidate = None
                continue
            member = members[candidate]
            target = types[member['type']]['target']
            target_size = types[target]['size']
            if not target_size: continue
            count = None
            if loader == 'Load_Stream' and args[0] == 'AtStart' and len(args) == 3:
                count = f'({simplify(args[2], aliases)}) / {target_size}'
            elif loader.endswith('Array') and args[0] == 'AtStart' and len(args) == 2:
                element = loader[5:-5]
                expected = source_aliases.get(element, element[:-3] + ' *' if element.endswith('Ptr') else element)
                multiplier, divisor = 1, 1
                if expected != target:
                    if expected.removeprefix('const ') == target.removeprefix('const '): pass
                    elif types[target]['kind'] == 'array' and types[target]['element'] == expected:
                        divisor = types[target]['count']
                    elif element in strides:
                        multiplier, divisor = strides[element], target_size
                    else:
                        candidate = None
                        continue
                count = simplify(args[1], aliases)
                if multiplier != divisor: count = f'({count}) * {multiplier} / {divisor}'
            elif args[0] in ('AtStart', 'NotAtStart') and len(args) == 1:
                element = loader[5:]
                if element.endswith('Ptr'): element = element[:-3]
                if target != element:
                    candidate = None
                    continue
                count = '1'
            if count is None:
                candidate = None
                continue
            try:
                extent = expression(count, types)
            except (ValueError, SyntaxError, StopIteration, KeyError):
                rejected.append(dict(type=name, member=candidate, expression=count, reason='unresolved_expression'))
                candidate = None
                continue
            rule = dict(count=extent, evidence='game_test_loader', source_file=source['file'],
                        source_line=source['line'] + code.count('\n', 0, position), expression=count)
            found.setdefault(candidate, []).append(rule)
            candidate = None
        for member, observations in found.items():
            if len({json.dumps(r['count'], sort_keys=True) for r in observations}) != 1:
                rejected.append(dict(type=name, member=member, reason='conflicting_extents'))
            else:
                path = members[member]['path']
                owner_rules = rules.setdefault(name, {})
                if len(path) == 1:
                    owner_rules[member] = observations[0]
                else:
                    member_type = next(m['type'] for m in type['members'] if m['name'] == path[0])
                    container = owner_rules
                    key = path[0]
                    for index in path[1:]:
                        array = types[member_type]
                        missing = key not in container if isinstance(container, dict) else container[key] is None
                        if missing:
                            container[key] = dict(elements=[None] * array['count'])
                        container = container[key]['elements']
                        key = index
                        member_type = array['element']
                    container[key] = observations[0]
    # These fixed arrays call a pointer loader once per slot. That loader
    # serializes one record per non-null pointer, unlike the counted arrays
    # above. Do not generalize this to arbitrary pointer arrays.
    for owner, member, source_name, loader in [
        ('WeaponDef', 'notifyTypes', 'WeaponDef', 'WeaponEntityNotifyPtr'),
        ('MaterialPipelineState', 'serializedShaders', 'MaterialPipelineState', 'MaterialSerializedShaderPtr'),
        ('MapEnts', 'dynEntSpatialPopulation', 'MapEnts', 'SpatialPartition_PopulationPtrArray'),
        ('MapEnts', 'dynEntSpatialTransientMap', 'MapEnts', 'SpatialPartition_Population_TransientMapPtrArray'),
        ('DynEntityList', 'dynEntSpatialPopulation', 'DynEntityListArray', 'SpatialPartition_TransientPopulationPtrArray')]:
        if owner not in types or source_name not in sources: continue
        source = sources[source_name]
        assert f'->{member}' in source['code'] and f'Load_{loader}(' in source['code']
        array_type = types[next(m['type'] for m in types[owner]['members'] if m['name'] == member)]
        assert array_type['kind'] == 'array' and types[array_type['element']]['kind'] == 'pointer'
        rules.setdefault(owner, {})[member] = dict(elements=[dict(count=1, evidence='game_test_pointer_loader',
            source_file=source['file'], source_line=source['line']) for _ in range(array_type['count'])])
    if 'MaterialTechniqueSet' in types and 'MaterialTechniqueSet' in sources:
        source = sources['MaterialTechniqueSet']
        assert '__popcnt(varMaterialTechniqueSet->techniqueMask.mask[' in source['code']
        counts = [dict(op='popcount', args=[dict(owner='MaterialTechniqueSet', path=['techniqueMask', 'mask', i])])
                  for i in range(4)]
        count = dict(op='add', args=[dict(op='add', args=counts[:2]), dict(op='add', args=counts[2:])])
        rules.setdefault('MaterialTechniqueSet', {})['maskedTechniques'] = dict(count=count,
            evidence='game_test_loader_and_native_replay_E0D380_popcount64', source_file=source['file'],
            source_line=source['line'], native_rva=0xe0d380)
    for owner, member, loader in [
        ('DLogChannel', 'serializers', 'DLogSerializerPtr'),
        ('DLogChannel', 'postSerializers', 'DLogSerializerPtr'),
        ('MaterialTechniqueSet', 'maskedTechniques', 'MaterialTechniquePtr'),
        ('WeaponDef', 'weaponOffsetPatternsKickOrSnapDecay', 'WeaponOffsetPatternDescriptionPtr'),
        ('ClientTriggers', 'linkTo', 'ClientEntityLinkToDefPtr'),
        ('tacpoint_search_node_t', 'm_ChildNodes', 'tacpoint_search_node_ptr')]:
        if owner not in types or loader not in sources: continue
        source = sources[loader]
        assert 'Load_Stream(streamStart,' in source['code'] and 'AtStart' in source['code']
        rules[owner][member]['pointee'] = dict(count=1, evidence='game_test_single_record_pointer_loader',
            source_file=source['file'], source_line=source['line'])
    rules.setdefault('RawFile', {})['buffer'] = dict(count=dict(op='choose_nonzero', args=[
        dict(owner='RawFile', path=['compressedLen']), dict(owner='RawFile', path=['len'])]),
        evidence='native_rawfile_lengths', expression='compressedLen ? compressedLen : len')
    for owner, member, length in [('GfxComputeShaderLoadDef', 'program', 'programSize'),
                                   ('LuaFile', 'buffer', 'len'), ('SoundBankListDef', 'soundBankNames', 'soundBankCount')]:
        rules.setdefault(owner, {})[member] = dict(count=dict(owner=owner, path=[length]),
                                                   evidence='native_payload_layout', expression=f'{owner}.{length}')
    # These loaders use a helper or a cast of the enclosing structure instead
    # of the direct member/array patterns above. Keep their evidence explicit.
    for owner, member, count, source_name in [
        ('DLogChannelRef', 'channel', 1, 'DLogChannelRef'),
        ('StreamingItemContents', 'contents', dict(indexed_count=dict(owner='StreamingContentSet', items='contents',
            item_type='StreamingItemContents', counts='contentCounts', limit=dict(owner='TransientCosts', path=['transientCostCount']))),
         'StreamingItemContents'),
        ('AlwaysloadedFlags', 'imageFlags', dict(op='shr', args=[dict(op='add', args=[
            dict(op='mul', args=[4, dict(owner='AlwaysloadedFlags', path=['imageCount'])]), 31]), 5]), 'AlwaysloadedFlags'),
        ('GfxImage', 'packedAtlasData', dict(op='select', args=[dict(op='and', args=[
            dict(owner='GfxImage', path=['flags']), 512]), dict(owner='GfxImage', path=['atlasInfo', 'packedAtlasDataSize']), 0]),
         'GfxImagePackedAtlasData')]:
        if owner not in types or source_name not in sources: continue
        source = sources[source_name]
        rules.setdefault(owner, {})[member] = dict(count=count, evidence='game_test_loader_and_helper',
            source_file=source['file'], source_line=source['line'])
    reference_rules.apply(rules, types)
    return rules, rejected


if __name__ == '__main__':
    schema = json.loads((ROOT / 'config/data/mw19/schema.json').read_text())
    sources = json.loads((ROOT / 'research/mw19/source_loaders.json').read_text())
    rules, rejected = generate(schema, sources)
    (ROOT / 'research/mw19/pointer_rules.json').write_text(json.dumps(dict(rules=rules, rejected=rejected), indent=2) + '\n')
    print(json.dumps(dict(types=len(rules), rules=sum(map(len, rules.values())), rejected=len(rejected))))
