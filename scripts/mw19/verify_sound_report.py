"""Create a small synthetic SAB or verify one reported sound in memory."""
import argparse
import io
import json
import struct
import subprocess
import sys
import zlib
from pathlib import Path

FRAMES = bytes.fromhex('fff86908000f30001234aa3bfff86908010414001234817c')


def fixture_bank(two=False):
    bank = bytearray(1024)
    struct.pack_into('<8I3Q', bank, 0, 0x23585532, 10, 44, 16, 64, 1, 0, 16, 1024, 688, 752)
    struct.pack_into('<5IQIBBB', bank, 688, 0x1234, 24, 4, 21, 0, 800, 44100, 1, 1, 8)
    bank[804:828] = FRAMES
    if two:
        struct.pack_into('<I', bank, 20, 2)
        struct.pack_into('<Q', bank, 48, 900)
        bank[732:776] = bank[688:732]
        struct.pack_into('<I', bank, 732, 0x1235)
        bank[766] = 9
    return bytes(bank)


def fixture_flac():
    return (b'fLaC\x80\0\0\x22' + struct.pack('>HH', 16, 16) + bytes(6) +
            ((44100 << 44) | (15 << 36) | 21).to_bytes(8, 'big') + bytes(16) + FRAMES)


def verify(path, executable, process=False, negative=False):
    path = Path(path)
    report = json.loads(path.read_text())
    assert report['test_only'] and report['complete']
    if process:
        assert {p.name for p in path.parent.iterdir()} == {'manifest.json', 'assets.jsonl', 'progress.json'}
        assert report['audio_requested'] and report['partials'] == (2 if negative else 0)
        assert report['failures'] == int(negative) and len(report['assets']) == (4 if negative else 1)
        good = report['assets'][0]
        assert good['status'] == good['payload_status'] == good['audio_status'] == 'ok'
        assert good['payload_bytes'] == 1024 and good['payload_crc32'] == zlib.crc32(fixture_bank(two=True))
        assert good['audio_total_samples'] == 2 and len(good['audio_samples']) == 1
        sample = good['audio_samples'][0]
        assert sample['bytes'] == 66 and sample['crc32'] == zlib.crc32(fixture_flac())
        assert not any(k.endswith('_file') for a in report['assets'] for k in a)
        assert not any('file' in s for a in report['assets'] for s in a.get('audio_samples', []))
        if negative:
            codec, corrupt, missing = report['assets'][1:]
            assert codec['audio_status'] == 'unavailable' and codec['payload_status'] == 'ok'
            assert codec['audio_samples'][0]['reason'] == 'sound_codec_unavailable'
            assert corrupt['audio_status'] == 'failed' and corrupt['payload_status'] == 'ok'
            assert 'FLAC' in corrupt['audio_samples'][0]['error']
            assert missing['payload_reason'] == 'sound_bank_not_loaded' and missing['audio_status'] == 'unavailable'
        print('Sound pool report verified: exact SAB/FLAC CRCs, one sample per bank, explicit failures; no asset files.')
        return
    assert report['success']
    # Run the C++ sound reader into a pipe, never a FLAC output file.
    flac = subprocess.check_output([str(executable), '--extract', report['archive'], str(report['key'])])
    assert report['payload_bytes'] == len(flac) and report['payload_crc32'] == zlib.crc32(flac)
    expected_reads = 688 + 44 * report['indexed_entries'] + report['sample']['encoded_bytes']
    assert report['file_bytes_read'] == expected_reads
    sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'build/python'))
    import soundfile as sf
    with sf.SoundFile(io.BytesIO(flac)) as decoded:
        sample = report['sample']
        assert (decoded.frames, decoded.samplerate, decoded.channels) == (
            sample['frames'], sample['sample_rate'], sample['channels'])
        pcm = bytes(decoded.buffer_read(decoded.frames + 1, dtype='int16'))
        assert len(pcm) == decoded.frames * decoded.channels * 2
        if report['key'] == 0x1234:
            assert flac == fixture_flac() and pcm == b'\x34\x12' * 21
        if report['key'] == 0x35ac3fc3:
            assert zlib.crc32(pcm) == 0x373224a3
        print(json.dumps(dict(flac_bytes=len(flac), flac_crc32=zlib.crc32(flac),
            frames=decoded.frames, rate=decoded.samplerate, channels=decoded.channels,
            pcm_bytes=len(pcm), pcm_crc32=zlib.crc32(pcm), decoder='libsndfile via soundfile',
            asset_files_written=0)))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['create', 'verify'])
    parser.add_argument('path', type=Path)
    parser.add_argument('--executable', type=Path)
    parser.add_argument('--process', action='store_true')
    parser.add_argument('--negative', action='store_true')
    args = parser.parse_args()
    if args.action == 'create': args.path.write_bytes(fixture_bank())
    else: verify(args.path, args.executable, args.process, args.negative)
