# Replay fastfile exporter

`fastfile -r mw19replay` loads and exports MW2019 **1.20.4.7623265 Replay** fastfiles. All **111 enabled asset types** have native loader bindings: **19 conventional payload exporters** and **92 structured exporters**. The [README lists every supported type](../../README.md#mw2019-replay-additions-in-this-fork). The 117-pool catalog also contains the disabled `iesprofile` and five unused IDs; those do not have Replay fastfile loaders.

## Bounded capability test

Supply your matching local `game_dx12_ship_replay.exe` and, for Oodle-compressed data, the game's absolute Oodle DLL path:

```powershell
acts.exe --noUpdater fastfile -r mw19replay `
  -g "D:/Replay/game_dx12_ship_replay.exe" `
  --oodle "D:/Replay/oo2core_7_win64.dll" `
  --test --limit-per-pool 1 `
  -o output/replay-test "D:/Replay/zone/code_pre_gfx.ff"
```

This fully loads the zone, then tests at most one real asset per type. `--limit-per-pool` accepts 1–32; its default is 1. Name-only dependencies beginning with a comma do not consume the quota. `--test` encodes and checksums output in memory and writes **only `manifest.json` and `assets.jsonl`**, under `output/replay-test/mw19replay/<zone>/`. It cannot be combined with decompressed-file, binary, string-table dump options or asset-pool mode. `--noAssetDump` performs loader/inventory validation without exercising exporters.

Use `-a "weapon,material,rawfile"` to select types, and `-n "name1,name2"` for exact asset names or ACTS hash patterns. Selection limits export; it does not skip loading the other serialized assets needed to fix pointers. Multiple input files and directory input use ACTS's usual fastfile reader workflow.

Streamed image parts, model buffers and stream keys may be stored in XPaks. Supply the matching archives explicitly:

```powershell
acts.exe --noUpdater fastfile -r mw19replay `
  -g "D:/Replay/game_dx12_ship_replay.exe" `
  --oodle "D:/Replay/oo2core_7_win64.dll" `
  --xpak "D:/Replay/zone/pak_launch_remaster_1.xpak" `
  --test -o output/shipment-test "D:/Replay/zone/mp_shipment.ff"
```

`--xpak` is repeatable, with at most 64 archives. Later archives take priority for duplicate keys. Reports record the archive, key, decoded size and CRC32 for each retrieved payload. Missing keys are recorded with their expected byte length, and return a nonzero result. A fastfile does not supply bytes that live only in an absent XPak.

## Export files

Remove `--test` to export all selected assets. Files are grouped as `mw19replay/<zone>/assets/<type>/`, retaining engine names and a unique record suffix. Invalid Windows filenames receive an encoded path, with their original names retained in the journal. Equal names within a zone do not overwrite one another.

| Asset types | Output |
| --- | --- |
| `rawfile`, `luafile`, `ttf` | Original payload bytes; rawfile zlib data is decompressed |
| `scriptfile` | GSCBIN stack/bytecode container |
| `stringtable` | CSV |
| `localize`, `netconststrings`, `soundbanklist` | JSON |
| Six shader types | Compiled program bytes (`.cso`), or `.shader.json` for a valid empty shader declaration |
| `image` | DDS with DX10 header, including resident pixels retained by this loader |
| `xmodelsurfs`, `streamkey` | Shared geometry / stream buffer bytes |
| `xmodelsurfs` with `--geometry` | Additional `.geometry.glb` with base surface positions, normals, UVs and triangles |
| `soundbank`, `soundbanktransient` | Loaded SAB bytes; the original index/checksum order is preserved |
| Other 92 enabled types | Versioned structured `.asset.json` |

Structured exports contain the field graph, named references to other assets, owned arrays, and labelled runtime references. Pointer-free arrays with 4,096 or more elements use `encoding: "hex-little-endian"`, `type`, `count`, `stride`, and complete hexadecimal `bytes`. This retains scalar bit patterns and union views without producing millions of repeated JSON nodes. The bundled `data/mw19/schema.json` describes the element layout. Runtime GPU records remain explicitly opaque.

`script_strings.json` preserves the zone's script-string table in index order. Null entries remain null; invalid UTF-8 strings use `string_bytes` hex records. Structured fields retain the original fastfile indices, which resolve through this table. Test mode checks its checksum in memory and does not write the table.

The manifest distinguishes `complete` (the load/export pass finished) from `success` (no attempted export failed or lacked required data). The journal records per-asset status, format, length and CRC32. Structured failures include field paths and reasons. A failed loader leaves `complete: false` with the last top-level index and consumed byte count. Missing required resident data is reported as unavailable. A shader with both a null program and zero program length is a valid declaration: its name, type, flags and `empty_shader` status are exported as JSON. A nonzero program length with no program bytes is still unavailable. Atomic manifest replacement retries brief Windows reader-sharing conflicts; a persistent failure preserves the previous manifest and includes the Windows error code.

## Serve mode

A one-shot run maps the executable, reads the schema, then reads, decompresses, patches and loads the whole zone before it exports anything, and it does all of that again for the next request on the same zone. A tool that asks for a few assets at a time (an editor filling its texture and model caches) should start one process with `--serve` instead:

```powershell
acts.exe --noUpdater fastfile -r mw19replay `
  -g "D:/Replay/game_dx12_ship_replay.exe" `
  --oodle "D:/Replay/oo2core_7_win64.dll" `
  --xpak "D:/Replay/zone/pak_launch.xpak" `
  --serve --serve-cache-mb 2048 -l
```

The process answers requests until its standard input closes. It reads **one JSON object per line** and writes **one line starting with `@acts ` followed by a JSON object** for each request, after a first `@acts {"ready":true,"protocol":1,...}` line. Every other output line is ordinary log text. `-g`, `--oodle` and `--xpak` are given once on the command line; a fastfile named on the command line is loaded and kept before the first request, and nothing is exported for it.

| Request | Fields | Effect |
| --- | --- | --- |
| `export` (default `cmd`) | `zone` (fastfile path), `out` (output folder), optional `types` (as `-a`), `names` (array, or a string as `-n`), `geometry`, `inventory` (as `--noAssetDump`), `test`, `limit`, `script_strings` | Exports the selection under `<out>/mw19replay/<zone>/`, with the same files, journal and manifest as a one-shot run |
| `load` | `zone` | Loads and keeps the zone; writes nothing |
| `stats` | | Lists the zones in memory with their sizes |
| `trim` | | Releases every zone |
| `ping`, `quit` | | Liveness check; orderly exit |

Every request may carry an `id` of any JSON type; the answer repeats it. An `export` or `load` answer holds `ok` (the pass completed: the manifest's `complete`), `cached` (the zone was already in memory), `loaded`, `tested`, `failed`, `unavailable`, `references`, `dir`, `zones` (what is in memory now, most recently used first), `ms`, and `error` when something went wrong. An export with unavailable payloads answers `ok: true` with the same `error` text a one-shot run logs; the journal has the per-asset reasons. A request that cannot be parsed, names an unknown pool or a file that cannot be read answers `ok: false` and the process continues.

`script_strings.json` is only written when a request sets `script_strings: true` (a one-shot run always writes it). Names are passed as a JSON array, so a name never needs shell quoting; a name that contains a comma is refused, as `-n` cannot express it either.

Loaded zones stay in memory, most recently used first, until their total exceeds `--serve-cache-mb` (default 2048 MiB; the bytes counted are the stream bytes the loader wrote plus the retained roots and pixels). The least recently used zones are then released; the zone just used is always kept, also when it alone is larger than the budget. A zone is identified by its path, size and modification time and those of its `.fp` / `.fc` patches, so a rebuilt file is loaded again. `--serve` cannot be combined with `--trace-replay-streams`, decompressed dumps, asset-pool mode or `ACTS_REPLAY_PRELOAD`.

`scripts/mw19/test_serve.py` exports the same selection by a one-shot run, a first serve request and a repeated one, compares the three trees and exercises the commands above.

## Speed

Measured on Replay 1.20 (i7-12700F, NVMe, files in the system cache, 14 XPaks on the command line where streamed payloads are needed). "Before" is the same build without the changes listed below. The machine was in use during the runs; times vary by tens of percent.

| Run | Before | One-shot | Serve, zone loaded |
| --- | ---: | ---: | ---: |
| 3 material bodies, `techsets_mp_backlot2` (2 MB) | 0.85 s | 0.20 s | 4 ms |
| 6 streamed images, `mp_backlot2` (156 MB, patched) | 3.2 s | 0.44 s | 60 ms |
| 3 xmodels, `common_base_mp` (127 MB, patched) | 5.3 s | 0.40 s | 12 ms |
| Load only, `common_mp` (712 MB, patched) | 9.4 s | 1.5 s | 4 ms |
| `gfx_map` + `gfx_map_trzone`, `mp_shipment` (370 MB of JSON) | 9.3 s | 2.0 to 2.8 s | |
| Every asset of `code_post_gfx` (6,013 files) | 20.0 s | 4.2 s | |
| `material` + `techset`, `techsets_mp_shipment` (1,320 files) | 3.8 s | 0.43 s | |
| Images, models, surfaces, raw files of `mp_backlot2` (6,957 files, 4.7 GB) | 66.9 s | 37.4 s | |

What changed: the stream hooks no longer build a trace event unless a trace is written; `manifest.json` is rewritten a few times per second instead of every 256 assets; loader memory is read directly instead of through `ReadProcessMemory`; fastfile blocks and patch windows are decoded on several threads into buffers that are not cleared first; the schema is indexed by name; the selected records are built and written on up to eight threads, with journal lines, counters and file names in record order as before; structured JSON is written by a compact writer that copies strings which need no escaping (the hex text of a world map's arrays); XPak payloads are read through a file mapping, without a shared file position. The last table row is bound by the disk: 4.7 GB are read from the XPaks and written again.

`manifest.json` records where a run spent its time in `timing_ms`, in milliseconds: `init` (with `init_schema`, `init_module`, `init_packages`), `read`, `decompress`, `patch`, `load` and `export`. `export_encode` and `export_write` are summed over the export threads, so they can exceed `export`. `stream_bytes` gives the stream reservations, the bytes loading wrote and the retained copies.

## Version and format boundaries

Use `--geometry -a xmodelsurfs` to create GLB sidecars for a 3D viewer. Keep `--test` to encode and validate them only in memory. The journal records geometry status, surface/vertex/triangle counts, length, CRC32 and, for a disk export, `geometry_file`. This is base surface geometry; it does not assemble XModel LODs, assign game materials/textures, or implement skinning, morphs or subdivision. The matching XPak is still required for a streamed shared buffer.

The Replay reader accepts the revisions present in the 1.20 install: **`0xfcd`, `0xfcf`, `0xfd0`, `0xfd1`, `0xfd5`, `0xfda`, `0xfe1`, `0xfe3`, `0xfee`, `0xff3`, `0xff5`, and `0xff7`**. Unchanged transient zones can retain an older revision. These use eleven header reservations and eight native streams; the effective revision is recorded in `manifest.json` as `xfile_version` and the base revision as `input_xfile_version`. Stream bounds, asset checks, and complete serialized-byte consumption remain enforced. Other revisions still fail explicitly.

The **`mw19replay` reader automatically applies adjacent `.fp` and then `.fc` patches** when present. ACTS checks each embedded previous header against its actual input; a mismatched, malformed, or unreadable local patch fails explicitly. The manifest records `input_xfile_version`, final `xfile_version`, and `applied_patches`. Use **`--no-auto-patch`** only to investigate a base file deliberately; explicit `-p`/`--fc` still apply the requested stages with that option. Other game readers retain their existing explicit patch behavior.

Header-only Replay patches that change only the version retain the base serialized payload; a patch that implies different payload data must supply its delta. If a later native asset load fails, completed root records are still exported and the manifest remains explicitly incomplete with `partial_load: true` and the loader error. This permits inspection of available assets without claiming the entire zone loaded.

The owner supplies the Replay executable. ACTS validates the catalog and 111 loader-entry signatures before redirecting stream operations and asset registration. It maps the executable without calling its entrypoint or resolving its imports. Serialization follows the native generated loaders; bounded replacement streams handle alignment, temporary rewinds, shared data, relative/packed offsets, pointer aliases and insertions. Asset headers are retained before temporary storage is reused. The image-upload callback copies resident pixels before stream 1 rewinds; copying only the image header would leave subsequent images overwriting those pixels. Other GPU upload and runtime registration finish hooks are omitted after serialized data is read. No executable, proprietary source, decompilation or asset is included in this repository.

Populated Replay layouts override the game-test source where the executable differs: `DDLMember` is 64 bytes, `PlayerAnimsetState` is 64 bytes with 48-byte condition records, `GfxVoxelTree` is 112 bytes, and `GfxSurfaceBounds` is 56 bytes. Surface bounds preserve the additional 32 serialized bytes as `serializedExtra`; their scalar meanings remain unnamed. Counted pointers include the profile-resolved `stride`, including expanded arrays. The `0xfda` adapter handles the older `ComWorld`, 112-byte `ScriptableDef`, and the 17,744-byte frontend `GfxWorld`, including its 96-byte per-light meshes, 32-byte mesh vertices, 48-byte view frustums, 16-bit frustum indices and 31-entry visibility arrays. It restores the original native lighting loader when a subsequent file uses another revision.

For that older world, `asset.serialized_layout` preserves the original unrelocated root and lighting records alongside normalized fields, including complete 32-byte light-mesh vertices. Serialized records whose scalar semantics remain unnamed are labelled `opaque_serialized_record`, separately from runtime-only records. This is not a claim that every field in every historical layout has been identified: a populated older dynamic-lightset region still fails explicitly pending its matching layout. [Parser validation notes](parser-validation.md) document the measured layouts, tests and current input limits.

Default hard limits include 2 GiB of stream reservations, 128 MiB of retained asset headers, 512 MiB of retained resident/legacy payloads, one million top-level assets/script strings, 128 MiB per conventional payload, 256 MiB per structured payload/read budget, four million traversal nodes, 32 million array elements and depth 64. The loader reports exceeded limits rather than claiming a partial asset is complete. Large zones still require enough memory for their full decompressed stream and reservations during a bounded test.

This is an exporter. Structured JSON and shared geometry buffers are not a complete model interchange format or a round-trip zone specification. The existing [IW8 linker](linker.md) still writes its eight supported concrete types. These changes do not add GSC compilation/decompilation or establish live unsigned-zone loading.

## Validation

Build `AtianCodToolsCLI` in Release first. The reusable regression suite only writes **synthetic** asset files:

```powershell
python scripts/mw19/verify_fastfile_bindings.py "D:/Replay/game_dx12_ship_replay.exe"
python scripts/mw19/test_fastfile.py "D:/Replay/game_dx12_ship_replay.exe" --out build/mw19-tests/fastfile-run
python scripts/mw19/test_fastfile_geometry.py "D:/Replay/game_dx12_ship_replay.exe" --out build/mw19-tests/geometry-run
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/mw19/test_schema.ps1
```

Use a new directory for `--out`. The fastfile suite tests all 111 native loaders, 108 successful isolated empty-root exporters (including valid shader declarations), explicit unavailable status for three empty conventional payloads, eight populated linker/exporter round trips, name/type filters, script strings, reference-only quotas, and malformed stream/count/pointer rejection. Populated regressions cover successive resident images sharing temporary storage, two 64-byte DDL members, and older frontend lighting and scriptable records. The C++ schema suite also tests Windows manifest-reader contention, populated player conditions and successive voxel trees. Empty-root tests establish dispatch and structural consistency; they do not prove every populated member or optional codec.

Bounded stock checks also loaded `code_pre_gfx.ff`, `techsets_common_mp.ff`, and `mp_shipment.ff`. Shipment consumed all **385,140,167 serialized bytes**, loaded **10,478 assets** from **8,848 top-level entries**, and successfully encoded one real sample from each of **51 types** with the matching model XPak: zero failed/unavailable samples. This included both world records, tactical graphs, map entities, weapons, material/technique records, resident DDS pixels, compiled shaders, a streamed model buffer and a loaded SAB. These tests kept all game payloads in memory and wrote only reports. They do not claim that every asset in every stock zone has been tested.

A subsequent run raised the cap to three samples per type and supplied the 14 nonempty Replay XPaks: **122 successful samples across 56 types** (5 in `code_pre_gfx`, 17 in `techsets_common_mp`, and 100 in Shipment), with zero failures or unavailable payloads. Only six report files were written. Independent Python/Oodle decoding also matched the sampled mesh's 85,168 bytes and CRC32 `de4b13ed`. Shipment's 8,390-entry script-string table was validated in memory.

The optional fastfile GLB path also passed a populated synthetic triangle through the native loader, file export, independent position/CRC checks, report-only mode and invalid-index rejection. Three Shipment model-surface samples passed `--test --geometry`: 2,501/1,094/544 vertices and 2,838/850/340 triangles. Their GLBs were encoded in memory; only the two capability reports were written.

## Research switches (opt-in)

These environment variables are for research. They are not part of normal exports. Each one is off unless set. Unset them before producing exports for others or regression baselines.

**`ACTS_REPLAY_STRUCTURED`** exports Replay assets as structured JSON instead of binary payloads. Only the value `1` enables it. Assets with a payload exporter, such as sound banks, export their structure instead. Structured fields with read errors are kept rather than rejected, and the issues remain listed in the manifest. Size limits rise to 2047 MiB and 60 million traversal nodes. The output uses the existing `manifest.json`, journal and field files. It creates no new file names.

**`ACTS_REPLAY_PAYLOAD_MB`** sets the conventional payload budget for `mw19pools` exports. Accepted values are whole numbers from `128` to `4096`. Unset, empty, non-numeric, and out-of-range values use the 128 MiB default. The variable does not change the Replay exporter, which keeps its 128 MiB budget. It creates no new file names.

**`ACTS_NATIVE_BYTES`** adds a `$native` key to each struct and union in structured inspection output. Its value is the exact loaded bytes of that struct as hex. Set it to `1`. Any value other than empty or `0` also enables it. Failed reads leave the key out. It creates no new file names, but the field JSON files grow.

**`ACTS_REPLAY_PRELOAD`** loads each Replay asset through `Preload_XAssetHeader`, the path used for map zones, and records the postload stream plan. Set any value to enable it, including `0`, so leave it unset for normal use. The run writes `preload-plan.jsonl` in the Replay output directory. Each line holds `entry`, `xasset_index`, `type_id`, `caller_rva` and per-block `deltas`. A completed run also adds `preload_plan`, `preload_plan_entries` and `preload_plan_block0_entries` to `manifest.json`, and logs a `Replay preload:` summary line.
