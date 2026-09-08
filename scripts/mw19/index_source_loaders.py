"""Index game-test load routines for local, reproducible layout research."""
import argparse
import json
import re
from pathlib import Path


def extract(directory):
    result = {}
    allocators = {}
    signature = re.compile(r'^void Load_(\w+)\(const DBStreamStart streamStart[^\n]*\)\s*\n\{', re.M)
    for path in sorted(Path(directory).glob('*.h')):
        text = path.read_text(encoding='utf-8')
        for allocation in re.finditer(r'^([^\n]+?)\s*AllocLoad_(\w+)\(\)\s*\n\{', text, re.M):
            returned = allocation[1].replace('__fastcall', '').strip()
            if returned.endswith('*'):
                target = returned[:-1].strip()
                target = re.sub(r'\s*\*\s*', ' *', target)
                allocators[allocation[2]] = target
        for match in signature.finditer(text):
            level, end = 1, match.end()
            while level:
                if text[end] == '{': level += 1
                if text[end] == '}': level -= 1
                end += 1
            result[match[1]] = dict(file=path.name, line=text.count('\n', 0, match.start()) + 1,
                                    code=text[match.end():end-1])
    result['__allocators__'] = allocators
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('directory')
    parser.add_argument('--output', default='research/mw19/source_loaders.json')
    args = parser.parse_args()
    result = extract(args.directory)
    Path(args.output).write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({'loaders': len(result)}))
