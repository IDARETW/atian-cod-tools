"""Independent reader for small, synthetic IW8 linker fixtures; no game access."""
import json
import struct
import sys
import zlib
from pathlib import Path


def lz4(data, expected):
    out = bytearray()
    pos = 0
    def length(n):
        nonlocal pos
        if n == 15:
            while True:
                b = data[pos]; pos += 1; n += b
                if b != 255: break
        return n
    while pos < len(data):
        token = data[pos]; pos += 1
        n = length(token >> 4)
        assert pos+n <= len(data) and len(out)+n <= expected
        out.extend(data[pos:pos+n]); pos += n
        if pos == len(data): break
        distance = int.from_bytes(data[pos:pos+2], 'little'); pos += 2
        n = length(token & 15)+4
        assert 0 < distance <= len(out) and len(out)+n <= expected
        for _ in range(n): out.append(out[-distance])
    assert len(out) == expected
    return bytes(out)


def unpack(path):
    data = path.read_bytes()
    assert data[:8] == b'IWffc100'
    assert struct.unpack_from('<II',data,8) == (11,0xff7)
    assert struct.unpack_from('<I',data,20)[0] == len(data)-136
    size = struct.unpack_from('<Q',data,32)[0]
    blocks = struct.unpack_from('<11Q',data,48)
    assert data[136:140] == b'\x02IWC'
    assert struct.unpack_from('<I',data,140)[0] == size
    chunk = int.from_bytes(data[144:147],'little'); codec = data[147]
    assert 0 < chunk <= 65536 and codec in (1,5)
    pos=148; body=bytearray(); count=0
    while len(body)<size:
        stored,raw,ctr=struct.unpack_from('<III',data,pos);pos+=12
        assert 0 < raw <= chunk and ctr == 0 and pos+stored <= len(data)
        part=data[pos:pos+stored];pos=(pos+stored+3)&~3
        body.extend(part if codec==1 else lz4(part,raw));count+=1
    assert len(body)==size and pos==len(data)
    return bytes(body),blocks,count


def inspect(body, blocks, records, schema):
    """Consume bytes in native load order and independently charge memory alignment."""
    pos=0; used=[0]*11
    def read(n, stream):
        nonlocal pos
        assert pos+n<=len(body)
        data=body[pos:pos+n];pos+=n;used[stream]+=n
        return data
    def align(stream,n): used[stream]=(used[stream]+n-1)&~(n-1)
    def string():
        n=body.index(0,pos)-pos
        return read(n+1,5)[:-1].decode()
    def u32(data,at): return struct.unpack_from('<I',data,at)[0]
    def u64(data,at): return struct.unpack_from('<Q',data,at)[0]
    follows=(1<<64)-2
    root=read(32,0)
    assert u32(root,0)==0 and u64(root,8)==0 and u32(root,16)==len(records) and u32(root,20)==0
    assert u64(root,24)==follows
    align(5,8); entries=read(len(records)*16,5)
    pools={p['name']:p for p in schema['profiles']['replay-1.20']['pools']}
    for i,a in enumerate(records):
        pool=pools[a['pool']]
        assert u32(entries,i*16)==pool['id'] and u64(entries,i*16+8)==(1<<64)-3
        align(1,8);align(5,8);used[5]+=8
        h=read(pool['size'],1);assert u64(h,0)==follows and string()==a['name']
        t=a['pool']
        if t=='rawfile':
            assert zlib.decompress(read(u32(h,8),5))==a['data'].encode()
            assert u32(h,12)==len(a['data'].encode())
        elif t=='luafile':
            align(5,16);assert read(u32(h,8),5)==a['data'].encode()
        elif t=='ttf':
            assert u64(h,24)==0 and read(u32(h,8)+1,5)==a['data'].encode()+b'\0'
        elif t=='localize': assert string()==a['value']
        elif t=='scriptfile':
            assert zlib.decompress(read(u32(h,8),6))==a['stack'].encode()
            assert read(u32(h,16),6)==a['bytecode'].encode()
        elif t=='soundbanklist':
            align(5,4);n=struct.unpack_from('<H',h,8)[0]
            assert list(struct.unpack('<'+'I'*n,read(n*4,5)))==a['hashes']
        else:
            if t=='stringtable':
                cols,rows,n=struct.unpack_from('<III',h,8)
                align(5,2);ids=struct.unpack('<'+'H'*(rows*cols),read(rows*cols*2,5))
                align(5,4);assert list(struct.unpack('<'+'I'*n,read(n*4,5)))==a['hashes']
            else: n=u32(h,20)
            align(5,8);assert read(n*8,5)==struct.pack('<Q',follows)*n
            strings=[string() for _ in range(n)]
            if t=='stringtable':
                assert [[strings[ids[c*rows+r]] for c in range(cols)] for r in range(rows)]==a['rows']
            else: assert strings==a['strings']
    assert pos==len(body) and tuple(used)==tuple(blocks)


def main():
    dest=Path(sys.argv[1]);schema=json.loads(Path(sys.argv[2]).read_text()); records=json.loads((dest/'fixtures.json').read_text())
    expected=(dest/'expected.body').read_bytes()
    results=[]
    for name in ['stored.ff','lz4.ff']:
        body,blocks,count=unpack(dest/name);assert body==expected and count>1
        inspect(body,blocks,records,schema)
        results.append({'file':name,'blocks':count,'body_bytes':len(body),'crc32':zlib.crc32(body)})
    (dest/'verification.json').write_text(json.dumps(results,indent=2))
    print('Independent IW8 stored/LZ4 decode, eight asset values, pointer markers, column-major table and exact stream reservations passed')

if __name__=='__main__': main()
