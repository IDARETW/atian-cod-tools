"""Regression checks for serialization-rule extraction, using small fixtures."""
import unittest
from generate_pointer_rules import array_strides, generate
from union_rules import switch_arms


class RuleExtractionTests(unittest.TestCase):
    def test_accumulator_cannot_retain_initial_zero_extent(self):
        types = {'int': dict(kind='scalar', size=4), 'int *': dict(kind='pointer', size=8, target='int'),
                 'Root': dict(kind='struct', size=8, members=[dict(name='data', type='int *')])}
        for mutation in ('v6 += unknown;', '++v6;', 'v6++;'):
            sources = {'Root': dict(file='fixture.h', line=1, code=f'''
                v6 = 0;
                varRoot->data = AllocLoad_int();
                {mutation}
                Load_intArray(AtStart, v6);
            ''')}
            rules, rejected = generate(dict(types=types), sources)
            self.assertNotIn('Root', rules)
            self.assertEqual(len(rejected), 1)
            self.assertEqual(rejected[0]['reason'], 'unresolved_expression')

    def test_nested_pointer_arrays_preserve_slot_counts_and_unknowns(self):
        types = {
            'int': dict(kind='scalar', size=4),
            'int *': dict(kind='pointer', size=8, target='int'),
            'int *[2]': dict(kind='array', size=16, count=2, element='int *'),
            'int *[2][2]': dict(kind='array', size=32, count=2, element='int *[2]'),
            'Root': dict(kind='struct', size=32, members=[dict(name='data', type='int *[2][2]')]),
        }
        sources = {'Root': dict(file='fixture.h', line=1, code='''
            v1 = varRoot;
            v1->data[0][0] = AllocLoad_int();
            Load_intArray(AtStart, 2);
            v1->data[0][1] = AllocLoad_int();
            Load_intArray(AtStart, 0);
            v1->data[1][0] = AllocLoad_int();
            Load_intArray(AtStart, 3);
        ''')}
        rules, rejected = generate(dict(types=types), sources)
        slots = rules['Root']['data']['elements']
        self.assertEqual([[v['count'] if v else None for v in row['elements']] for row in slots],
                         [[2, 0], [3, None]])
        self.assertEqual(rejected, [])

    def test_union_case_uses_enum_value_and_bound_type(self):
        members = [dict(name='first', type='A'), dict(name='second', type='B')]
        arms = switch_arms('case Later: varB = (B *)varUnion; break; case Earlier: varA = &varUnion->first; break;',
                           members, {'Earlier': 19, 'Later': 3})
        self.assertEqual(arms, [(3, 'second', {}), (19, 'first', {})])
        with self.assertRaises(ValueError):
            switch_arms('case Bad: varA = x; varB = y; break;', members, {'Bad': 0})
        with self.assertRaises(ValueError):
            switch_arms('case Earlier: case Later: varA = x; break;', members, {'Earlier': 19, 'Later': 3})

    def test_native_element_stride_not_allocator_return_type(self):
        sources = {
            'HalfArray': {'code': 'Load_Stream(streamStart, varHalf, 2 * count);'},
            'WordArray': {'code': 'Load_Stream(streamStart, varWord, count << 2);'},
            'BadArray': {'code': 'Load_Stream(streamStart, varBad, count + 4);'},
            '__allocators__': {'Half': 'unsigned __int8', 'Word': 'unsigned __int8'},
        }
        self.assertEqual(array_strides(sources), {'Half': 2, 'Word': 4})

    def test_string_member_cannot_acquire_later_array_extent(self):
        types = {
            'char': dict(kind='scalar', size=1), 'int': dict(kind='scalar', size=4),
            'char *': dict(kind='pointer', size=8, target='char'),
            'int *': dict(kind='pointer', size=8, target='int'),
            'Root': dict(kind='struct', size=24, members=[
                dict(name='name', type='char *'), dict(name='count', type='int'), dict(name='data', type='int *')]),
        }
        sources = {
            'Root': dict(file='fixture.h', line=1, code='''
                varXString = &varRoot->name;
                Load_XString(NotAtStart);
                Load_WordArray(AtStart, varRoot->count);
                v1 = varRoot;
                v1->data = AllocLoad_Word();
                Load_WordArray(AtStart, varRoot->count);
            '''),
            'WordArray': dict(file='fixture.h', line=10, code='Load_Stream(streamStart, varWord, 4 * count);'),
        }
        rules, rejected = generate(dict(types=types), sources)
        self.assertNotIn('name', rules['Root'])
        self.assertEqual(rules['Root']['data']['count'], dict(owner='Root', path=['count']))
        self.assertEqual(rejected, [])


if __name__ == '__main__': unittest.main()
