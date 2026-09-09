"""Synthetic IW8 forward/backward local-call decompilation regression."""
import argparse
from pathlib import Path
import struct
import subprocess
import zlib

ROOT = Path(__file__).resolve().parents[2]

def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('--acts', type=Path, default=ROOT / 'build/bin/Release/acts.exe')
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    # Function starts: 1, 8, 10. IW8's signed 24-bit local offsets have
    # one low tag bit. The call operands start at 3 and 12, respectively.
    code = bytes.fromhex('3b 4b4f0a0000583b 4b3b 4b4feaffff583b')
    stack = b''.join(struct.pack('<II', size, name) for size, name in [(7,100),(2,101),(7,102)])
    compressed = zlib.compress(stack)
    source = args.out / 'local_calls.gscbin'
    source.write_bytes(b'GSC\0' + struct.pack('<III',len(compressed),len(stack),len(code)) + compressed + code)
    output = args.out / 'source'
    result = subprocess.run([str(args.acts), '--noUpdater', 'gscd', '-g', '-v', 'iw8', '-f', 'iw',
                             '--path-output', '-o', str(output), str(source)], capture_output=True, text=True)
    (args.out / 'acts.log').write_text(result.stdout + result.stderr)
    text = (output / 'local_calls.gsc').read_text()
    assert result.returncode == 0 and '<errlocal:' not in text
    assert 'ref_0065();' in text and 'ref_0064();' in text
    print('Synthetic IW8 forward and backward local calls decompiled to the correct functions.')

if __name__ == '__main__': main()
