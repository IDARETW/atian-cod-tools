#!/usr/bin/env python3
"""Checks `fastfile -r mw19replay --serve` against one-shot exports of the same assets.

For every fastfile given, the selected asset types are exported three ways: by a one-shot run, by the first serve
request (which loads the zone) and by a repeated serve request (answered from memory). The three output trees must
be identical. Structured JSON prints load addresses, which differ between processes: they are masked, and the CRC32
of such a payload is left out of the journal comparison. Timing fields of the manifest are ignored.

Then the request protocol is exercised: an unknown pool, a missing file, a malformed line, a name that is absent,
`load`, `stats`, `trim` and `quit`.

The zones are read only. Everything is written under --out, which must be new or empty.

    python scripts/mw19/test_serve.py --game D:/Replay/game_dx12_ship_replay.exe \
        --oodle D:/Replay/oo2core_7_win64.dll --out build/mw19-tests/serve-run \
        D:/Replay/zone/code_pre_gfx.ff D:/Replay/zone/techsets_common_mp.ff
"""
import argparse
import hashlib
import json
import pathlib
import re
import subprocess
import sys
import time

ADDRESS = re.compile(rb'"address":"0x[0-9a-f]+"')
VOLATILE_MANIFEST = ("timing_ms", "cached", "script_strings_file")


def fail(message):
    print("FAIL:", message)
    sys.exit(1)


class Server:
    def __init__(self, command):
        self.log = []
        self.proc = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                     text=True, encoding="utf-8", errors="replace")
        self.ready = self.read()

    def read(self):
        while True:
            line = self.proc.stdout.readline()
            if not line:
                fail("the server closed its output; log tail: " + " | ".join(self.log[-6:]))
            line = line.rstrip("\r\n")
            if line.startswith("@acts "):
                return json.loads(line[6:])
            self.log.append(line)

    def send(self, text):
        self.proc.stdin.write(text + "\n")
        self.proc.stdin.flush()
        return self.read()

    def ask(self, **request):
        started = time.perf_counter()
        answer = self.send(json.dumps(request))
        answer["wall_ms"] = round((time.perf_counter() - started) * 1000, 1)
        return answer


def tree(root):
    """Relative path -> digest of every file below root, with the run-dependent parts removed."""
    digests = {}
    for file in sorted(root.rglob("*")):
        if not file.is_file():
            continue
        name = file.relative_to(root).as_posix()
        data = file.read_bytes()
        if name.endswith("manifest.json"):
            manifest = json.loads(data)
            for key in VOLATILE_MANIFEST:
                manifest.pop(key, None)
            data = json.dumps(manifest, sort_keys=True).encode()
        elif name.endswith("assets.jsonl"):
            lines = []
            for line in data.splitlines():
                record = json.loads(line)
                if record.get("format") == "mw19-asset-json-v1":
                    record.pop("payload_crc32", None)
                lines.append(json.dumps(record, sort_keys=True))
            data = "\n".join(lines).encode()
        elif name.endswith(".json"):
            data = ADDRESS.sub(b'"address":"X"', data)
        digests[name] = hashlib.sha1(data).hexdigest()
    return digests


def same(reference, other, label, without=()):
    a = {k: v for k, v in tree(reference).items() if not k.endswith(tuple(without))} if without else tree(reference)
    b = {k: v for k, v in tree(other).items() if not k.endswith(tuple(without))} if without else tree(other)
    problems = [f"missing {k}" for k in sorted(set(a) - set(b))]
    problems += [f"extra {k}" for k in sorted(set(b) - set(a))]
    problems += [f"differs {k}" for k in sorted(k for k in a if k in b and a[k] != b[k])]
    print(f"    {label}: {len(a)} file(s), {'identical' if not problems else 'DIFFERENT'}")
    for problem in problems[:8]:
        print("      " + problem)
    return not problems


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("zones", nargs="+", help="Replay fastfiles (.ff)")
    parser.add_argument("--acts", default="build/bin/Release/acts.exe")
    parser.add_argument("--game", required=True, help="game_dx12_ship_replay.exe (1.20 Replay)")
    parser.add_argument("--oodle", required=True, help="absolute path of the game's Oodle DLL")
    parser.add_argument("--xpak", action="append", default=[], help="streamed payload archive (repeatable)")
    parser.add_argument("--types", default="rawfile,stringtable,localize,material,techset",
                        help="asset types to export and compare")
    parser.add_argument("--names", default="", help="optional comma-separated asset names")
    parser.add_argument("--cache-mb", default="2048")
    parser.add_argument("--out", required=True, type=pathlib.Path)
    args = parser.parse_args()

    if args.out.exists() and any(args.out.iterdir()):
        fail(f"{args.out} is not empty")
    args.out.mkdir(parents=True, exist_ok=True)
    base = [args.acts, "--noUpdater", "fastfile", "-r", "mw19replay", "-g", args.game, "--oodle", args.oodle]
    for archive in args.xpak:
        base += ["--xpak", archive]
    names = [n for n in args.names.split(",") if n]
    zones = [pathlib.Path(z).as_posix() for z in args.zones]
    ok = True

    print("one-shot exports")
    for index, zone in enumerate(zones):
        command = base + ["-a", args.types, "-l", "-o", str(args.out / "oneshot" / str(index))]
        if names:
            command += ["-n", ",".join(names)]
        started = time.perf_counter()
        result = subprocess.run(command + [zone], capture_output=True, text=True)
        print(f"  {pathlib.Path(zone).name}: exit {result.returncode}, {round((time.perf_counter() - started) * 1000)} ms")

    server = Server(base + ["--serve", "--serve-cache-mb", args.cache_mb, "-l"])
    if not server.ready.get("ready") or server.ready.get("protocol") != 1:
        fail(f"unexpected greeting: {server.ready}")
    print("server:", server.ready)

    for attempt, folder in (("first request (loads the zone)", "serve-first"), ("repeated request (from memory)", "serve-again")):
        print(attempt)
        for index, zone in enumerate(zones):
            request = dict(id=index, zone=zone, types=args.types, out=str(args.out / folder / str(index)),
                           script_strings=True)
            if names:
                request["names"] = names
            answer = server.ask(**request)
            print(f"  {pathlib.Path(zone).name}: complete={answer.get('ok')} cached={answer.get('cached')} "
                  f"loaded={answer.get('loaded')} tested={answer.get('tested')} failed={answer.get('failed')} "
                  f"unavailable={answer.get('unavailable')} server {answer.get('ms')} ms")
            if answer.get("id") != index or answer.get("cached") is not (folder == "serve-again"):
                ok = False
                print("    unexpected answer:", answer)
            ok &= same(args.out / "oneshot" / str(index), args.out / folder / str(index), "against the one-shot export")

    print("protocol")
    first = zones[0]
    answer = server.ask(id="types", zone=first, types="not_a_pool", out=str(args.out / "bad-types"))
    print("  unknown pool ->", answer.get("ok"), answer.get("error"))
    ok &= answer.get("ok") is False and "not_a_pool" in str(answer.get("error"))
    answer = server.ask(id="file", zone=str(pathlib.Path(first).with_name("no_such_zone.ff").as_posix()), out=str(args.out / "bad-file"))
    print("  missing file ->", answer.get("ok"), answer.get("error"))
    ok &= answer.get("ok") is False
    answer = server.send("this is not a request")
    print("  malformed line ->", answer.get("ok"), str(answer.get("error"))[:60])
    ok &= answer.get("ok") is False
    answer = server.ask(id="absent", zone=first, types=args.types, names=["zz_no_such_asset_zz"], out=str(args.out / "absent"))
    print("  absent name ->", answer.get("ok"), "tested", answer.get("tested"), "cached", answer.get("cached"), answer.get("ms"), "ms")
    ok &= answer.get("ok") is True and answer.get("tested") == 0 and answer.get("cached") is True
    answer = server.ask(cmd="stats")
    print("  stats ->", [(z["zone"], z["assets"], round(z["bytes"] / 2**20)) for z in answer.get("zones", [])])
    ok &= len(answer.get("zones", [])) >= 1
    answer = server.ask(cmd="trim")
    emptied = server.ask(cmd="stats")
    print("  trim ->", answer.get("ok"), "zones left", len(emptied.get("zones", [])))
    ok &= answer.get("ok") is True and not emptied.get("zones")
    answer = server.ask(cmd="load", zone=first)
    print("  load ->", answer.get("ok"), "loaded", answer.get("loaded"), "zones", answer.get("zones"))
    ok &= answer.get("ok") is True and bool(answer.get("zones"))
    answer = server.ask(id="after-load", zone=first, types=args.types, out=str(args.out / "serve-after-load"), **({"names": names} if names else {}))
    ok &= answer.get("cached") is True
    ok &= same(args.out / "oneshot" / "0", args.out / "serve-after-load", "export after load, against the one-shot export",
               without=("script_strings.json",))
    answer = server.ask(cmd="quit")
    code = server.proc.wait(timeout=60)
    print("  quit ->", answer.get("ok"), "exit code", code)
    ok &= answer.get("ok") is True and code == 0

    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
