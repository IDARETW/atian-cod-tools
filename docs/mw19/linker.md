# IW8 Replay fastfile linker

`fastfilelinker` registers `IW8` and `MW19` as linker and compressor names. The target is **MW2019 1.20.4.7623265 Replay PC**, header version 11 and xfile version `0xff7`. The writer produces unsigned `IWffc100` base fastfiles with stored or LZ4 IWC blocks.

Eight concrete asset writers are implemented. Other asset classes fail before a file is emitted. The pool dumper's 117-pool mapping does **not** mean every pool can already be reconstructed into a fastfile. In particular, arbitrary `.asset.json` graphs, cross-asset dependencies, images/models/worlds, and runtime GPU resources are not linker inputs yet.

## Build a zone

Place the zone file and its inputs in one directory. For example, `example.zone`:

```text
>game=IW8
>name=example
>compression=lz4

rawfile,notes.txt
luafile,ui/example.lua
ttf,fonts/example.ttf
scriptfile,scripts/example.gsc.gscbin
localize,localize.json
stringtable,table.json
netconststrings,strings.json
soundbanklist,banks.json
```

Then run:

```powershell
acts.exe --noUpdater fastfilelinker -o output example.zone
```

This writes `output/zone/example.ff`. `>compression=none` uses stored blocks; `lz4` is the default. Outputs are deterministic for identical inputs and options. Assets are sorted by pool name and input name. Duplicate pool/name pairs are rejected. Asset names and all child strings must be NUL-free; binary fields retain NUL bytes.

The three binary file inputs use their relative paths as asset names. GSCBIN inputs use the name without the final `.gscbin` extension. JSON inputs carry their asset name explicitly.

| Pool | Input | Serialized content |
|---|---|---|
| `rawfile` | Any file | Zlib payload and exact original byte length |
| `luafile` | Existing source or bytecode file | Exact bytes; this command does not compile Lua |
| `ttf` | Existing font file | Exact bytes plus the loader's trailing NUL; FreeType handle cleared |
| `scriptfile` | GSCBIN from the dumper/gsc-tool | Validated compressed stack and existing bytecode; no GSC compilation |
| `localize` | JSON object | Name and localization value |
| `stringtable` | JSON object | Column-major cell indices, deduplicated strings and string hashes |
| `netconststrings` | JSON object | Type/source/flags and string list |
| `soundbanklist` | JSON object | Exact 32-bit bank hashes |

Example JSON inputs:

```json
{"name":"EXAMPLE_LABEL","value":"Example text"}
```

```json
{"name":"tables/example.csv","rows":[["key","value"],["example","42"]]}
```

Stringtable hashes use the IW8 `Com_HashStringLower` convention (32-bit multiply by 31, ASCII lowercase). The public IW8 x64-zt implementation supplies this algorithm reference; it is separate from the native Replay loader/array-layout evidence. An optional `hashes` array preserves supplied 32-bit hashes, one per unique cell in first-seen row order. Tables must be rectangular; the current bound is 65,535 cells.

```json
{"name":"example_strings","string_type":2,"source_type":0,"flags":0,"strings":["alpha","beta"]}
```

```json
{"name":"example_banks","hashes":[305419896,43981]}
```

Pool-specific enum values remain the author's responsibility; the linker validates integer width and sign. It does not invent dependency assets or resolve bank names to hashes.

## Format and validation

The serialized header contains eleven reservation slots at `0x30`; this does not imply eleven allocated retail streams. The writer uses Replay TEMP 0, TEMP_PRELOAD 1, VIRTUAL 5 and SCRIPT data stream 6. The root `XAssetList` is 32 bytes and each asset-list entry is 16 bytes. Inline pointers use `-2`, top-level insertion pointers use `-3`, and pointer insertion reserves eight virtual bytes without consuming serialized bytes. Alignment changes stream reservations without adding file bytes.

Replay loader RVAs for the implemented assets are recorded in the shipped schema. Native examples include rawfile `0xE04C70`, Lua `0xE04900`, TTF `0xE05750`, localization `0xE04780`, stringtable `0xE05520`, ScriptFile `0xDFE370`, NetConstStrings `0xDFDA50`, and SoundBankListDef `0xDFE570`. These bind root sizes, child ordering and the retail stream numbers; source-generated loaders supply the corresponding names and types. ScriptFile's stack and bytecode go to stream 6. Lua payloads align to 16, string pointer arrays to 8, 32-bit arrays to 4, and cell indices to 2.

The container uses an `02 I W C` marker, an eight-byte block-stream header, twelve-byte chunk headers and four-byte compressed-data alignment. No signing, encryption, patch-file generation or game policy changes are performed. Loading the unsigned output requires an environment that accepts unsigned custom fastfiles. No live Replay loading result is claimed.

Inputs and decompressed zone bodies are bounded to 128 MiB; strings to 65,535 bytes; assets to 65,535 entries. Encoding finishes before the final output is replaced. The command rejects malformed GSCBIN lengths/zlib data, unsupported asset types, unsafe paths, duplicates and invalid counts, and returns a nonzero exit status on failure.

## Small synthetic tests

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/mw19/test_linker.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/mw19/test_all_pools.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/mw19/test_fields.ps1
```

The linker suite creates tiny synthetic inputs under `build/mw19-tests`. Its independent Python reader verifies both stored and LZ4 multi-block containers, all eight asset values, pointer markers, column-major table layout and exact stream reservations. It checks the registered CLI writer and the existing ACTS decompressor, then verifies that an unsupported asset returns failure without writing a fastfile. The pool suites use a synthetic process and write only report files. These commands do not launch Replay or extract game assets.

The separate [Replay fastfile exporter suite](fastfile.md#validation) also loads these eight linked types through the matching executable's native serialization routines, exports them, and compares their values independently. That round trip does not establish live unsigned-fastfile acceptance or add linker serializers for the other pools.
