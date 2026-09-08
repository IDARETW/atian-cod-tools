"""Create bounded synthetic linker inputs; verify actual registered CLI output."""
import json
import struct
import sys
import zlib
from pathlib import Path
from verify_linker import unpack, inspect


def main():
    dest=Path(sys.argv[1])
    if '--verify-empty' in sys.argv:
        body,blocks,_=unpack(dest/'empty-out/zone/empty.ff')
        schema=json.loads((Path(__file__).resolve().parents[2]/'config/data/mw19/schema.json').read_text())
        inspect(body,blocks,[{'pool':'scriptfile','name':'empty','stack':'','bytecode':''}],schema)
        return
    if '--verify-cli' in sys.argv:
        body,blocks,count=unpack(dest/'cli/zone/synthetic.ff')
        assert body==(dest/'expected.body').read_bytes()
        schema=json.loads((Path(__file__).resolve().parents[2]/'config/data/mw19/schema.json').read_text())
        inspect(body,blocks,json.loads((dest/'fixtures.json').read_text()),schema)
        print('Registered IW8 linker/compressor output independently verified')
        return
    records=[
        {'pool':'rawfile','name':'test.txt','data':'Synthetic raw asset.\n'},
        {'pool':'luafile','name':'test.lua','data':'return 42'},
        {'pool':'ttf','name':'test.ttf','data':'SYNTHETIC FONT BYTES'},
        {'pool':'localize','name':'TEST_LABEL','value':'Synthetic localization'},
        {'pool':'scriptfile','name':'scripts/test.gsc','stack':'Synthetic stack\0','bytecode':'\x01\x02\x03\x04'},
        {'pool':'netconststrings','name':'test_strings','string_type':2,'source_type':0,'strings':['alpha','beta','alpha']},
        {'pool':'soundbanklist','name':'test_banks','hashes':[0x12345678,0xabcd]},
        {'pool':'stringtable','name':'test_table','rows':[['a','b','a'],['c','d','b']],'hashes':[97,98,99,100]},
    ]
    records.sort(key=lambda a:a['pool'])
    (dest/'fixtures.json').write_text(json.dumps(records))
    zone=['>game=IW8','>name=synthetic','>compression=lz4']
    for a in records:
        t=a['pool']
        if t in ['rawfile','luafile','ttf']:
            path=Path(a['name']); data=a['data'].encode()
        elif t=='scriptfile':
            path=Path(a['name']+'.gscbin');stack=a['stack'].encode();comp=zlib.compress(stack,9);code=a['bytecode'].encode()
            data=struct.pack('<IIII',0x435347,len(comp),len(stack),len(code))+comp+code
        else:
            path=Path(t+'.json'); data=json.dumps(a).encode()
        (dest/path).parent.mkdir(parents=True,exist_ok=True);(dest/path).write_bytes(data)
        zone.append(t+','+path.as_posix())
    (dest/'synthetic.zone').write_text('\n'.join(zone)+'\n')
    (dest/'bad.zone').write_text('>game=IW8\n>name=bad\nweapon,localize.json\n')
    (dest/'empty.gscbin').write_bytes(struct.pack('<IIII',0x435347,0,0,0))
    (dest/'empty.zone').write_text('>game=IW8\n>name=empty\nscriptfile,empty.gscbin\n')
    (dest/'corrupt.gscbin').write_bytes(struct.pack('<IIII',0x435347,2,500,0)+b'xx')
    (dest/'corrupt.zone').write_text('>game=IW8\n>name=corrupt\nscriptfile,corrupt.gscbin\n')
    (dest/'zone').mkdir(exist_ok=True)
    (dest/'zone/collision.ff').write_bytes(b'SYNTHETIC INPUT MUST SURVIVE')
    (dest/'collision.zone').write_text('>game=IW8\n>name=collision\nrawfile,zone/collision.ff\n')

if __name__=='__main__': main()
