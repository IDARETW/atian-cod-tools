# MW2019 Replay pool reader and asset dump support

The `mw19pools` command provides a versioned pool catalog, allocated-asset inventory, raw headers, structured field traversal, and format exporters. It uses a read-only Windows process handle and does not inject code or call game functions.

The current target is **1.20.4.7623265 Replay**, `game_dx12_ship_replay.exe`. The catalog contains **117 pool IDs**: 112 nonzero root structures, including the disabled IES pool, and five unused types. All 117 names, sizes, alignments, capacities, defaults and allocator bindings were checked against the executable. Native loader bindings exist for the 111 enabled types. See [the complete pool table](pools.md).

This is working dump infrastructure with explicit coverage reporting. It is **not yet a complete native-format exporter for every asset class**. Native root offsets and source-derived nested layouts have different evidence levels; an equal root size does not prove every nested member. Unresolved pointer extents and union selectors remain visible in reports.

## Report-only capability tests

When Replay is already running, validate a small sample without creating asset files:

```powershell
acts.exe --noUpdater mw19pools --test rawfile stringtable localize --limit-per-pool 1 -o output/mw19-test
```

`--test` reads headers, traverses fields, and exercises available payload exporters in memory. It defaults to one asset per selected loaded pool; the explicit limit accepts 1 through 32. Only `manifest.json`, `assets.jsonl`, and `progress.json` are written. The report records payload lengths and CRC32 values, separate header/field/payload statuses, read failures, unresolved fields, and exact output format identifiers. Up to 64 field issue paths and reasons are retained without copying asset values into the report. A loaded pool with no sampled asset supplies no live coverage.

Use `--pid` if there is more than one matching process. A process exit or allocation change leaves an explicit incomplete result. Enumeration takes a snapshot of the allocation bitmap and packed 20-byte entries; individual assets can still change after that snapshot, so test with zone loading idle.

The catalog can be inspected without a running game:

```powershell
acts.exe --noUpdater mw19pools --catalog -o output/mw19-catalog
```

`--profile game-test` selects the separate 113-pool source profile. It does not reuse Replay ordinals. For example, `stringtable` is Replay ID 54 and source ID 50.

## Export coverage

| Output | Coverage |
|---|---|
| Catalog | All 117 Replay pool records |
| Allocated-entry inventory | All pool IDs, including zone overrides and stashed entries |
| Raw root headers | Every nonzero pool, using its native size |
| Structured fields | Every nonzero root; versioned embedded layouts, 1,198 member rules across 392 source types, named asset references, bounded arrays and byte buffers |
| Selected union members | 26 selectors with 185 cases, including scriptable events/states, physics rules, particle modules, image/model residency, inactive stream-key hash state, animation tails, path trees and script-bundle root words |
| Payload exports | Raw files, GSC binary containers, Lua files, TrueType files, stringtable CSV, localization JSON, network strings, sound-bank hash lists, six shader stages, resident or XPak-backed Replay PC images as DDS, stream-key buffers, model shared buffers and loaded SAB sound banks |
| Optional geometry view | `xmodelsurfs` base surfaces as GLB, with positions, normals, first/second UV sets, vertex colors and triangle indices; skeleton and materials remain separate |

Conventional payload exporters cover 19 pools. The other 93 nonzero pools use the versioned `mw19-asset-json-v1` structured format (`.asset.json`), containing the field graph, bounded owned byte buffers, named asset references, and explicit runtime references. Its encoder runs in memory during `--test`; incomplete field graphs are rejected. JSON records are not native fastfile binaries. Invalid UTF-8 strings retain their original bytes in a labelled hexadecimal field. `--headers`, `--fields`, and `--payloads` select those outputs individually; `--dump` selects all three. These export options write assets, unlike `--test`. Each asset is keyed by its entry index so equal names in different zones do not overwrite each other. Names unsuitable for Windows paths receive an encoded filename while the original name stays in the manifest.

`ok` means the requested checks or exports succeeded for that sampled asset. `partial` identifies unresolved pointers/unions, an external streamed payload, or an unavailable payload. `failed` identifies a malformed/unreadable asset or failed output operation. Partial results and failures return a nonzero command status. `complete` describes whether the selected run finished; it does not certify full asset-format support.

Union traversal uses the owning record's selector and follows only the selected member. Value-only unions retain explicitly labeled views. Image and surface streamed handles are preserved without dereferencing them; nonzero handles increment `external_payloads`. Inline animation index tails use their actual frame count and byte/word width. Streaming content lengths are looked up in the containing set's parallel count array with index and read bounds.

Fixed arrays of pointers use individual loader extents, including nested arrays: 14 member rules cover 152 slots in weapon curves, entity lists, world visibility, spatial populations and shader records. Six pointer-list rules also follow the individual records behind each list entry. Zero-length slots perform no target read, an unreadable element does not abort its siblings, and slots without evidence remain unresolved. Replay's material-technique count is the population count of four 64-bit mask words, verified at native RVA `0xE0D380`; the generator invalidates mutated temporary values so a loop's initial zero cannot silently become the exported count.

`scripts/mw19/test_fields.ps1` checks two synthetic weapon records through the Windows process reader and writes only the three reports. It verifies successful structured traversal and a deliberately unreadable curve and accepts only the complete record for structured export. `scripts/mw19/test_schema.ps1` additionally checks unequal array extents, nested visibility arrays, unknown slots, technique-mask high bits, pointer-list records and cycles in memory.

For a static inventory of remaining traversal gaps, run `python scripts/mw19/audit_schema.py --output build/mw19-tests/schema-coverage.json`. This reads only the schema. It lists potential gaps per pool, including declared runtime descendants, and keeps pointer extents separate from union selectors. Rule presence alone does not prove every native member layout, payload codec or residency state.

Cell AABB trees use their corresponding `GfxCellTreeCount.aabbTreeCount` in the containing `GfxWorldDrawCells` table. The lookup checks the owning context, cell index, count-record field, count sign through traversal limits, and read bounds. Replay loaders `0xD96BB0` and `0xD908E0` confirm the 24-byte owner, four-byte count records, eight-byte tree records and 48-byte AABB records. A missing count array fails explicitly rather than selecting a guessed extent. DLog variable lists, disabled IES lookup data and dynamic bit arrays use separately labelled game-test field/count pairs.

Source-confirmed runtime references retain their addresses, reasons and evidence without following engine objects. Examples include FreeType faces, model-surface aliases, resolved sound caches, compiled animation trees and navigation list links. When applicable, `serialized_source` identifies the asset fields holding the original representation. Reports count these as `runtime_references`; they are distinct from unresolved pointers and unavailable payloads. The annotations are curated in `scripts/mw19/reference_rules.py`, not inferred automatically from unknown pointer types.

Visibility view 32 uses separately identified renderer extents: the static word counts, or 32 bytes per dynamic-entity word. It is not counted as an additional serialized view by the source loader. Known stream-key behaviors (terrain, sound, stream tree and clutter) select a runtime user-context pointer; behavior zero selects the asset hash, and unrecognized behavior values remain unresolved. The static audit currently finds no unclassified pointer slots along the exported field graph. This measures policy coverage only: it does not certify native layouts, every conditional traversal context, external residency or native-format conversion.

The current serializers do not retrieve nonresident shader programs or released nonstreamed image pixels. Complete model conversion, remaining sound codecs and some pointer extents still need additional native verification and serializers. GPU runtime records are retained as opaque record bytes. Fastfile linking coverage is documented below; additional GSC compiler/decompiler work remains outside this change.

## Replay PC image export

The DDS exporter covers the 46 usable pixel-format entries verified against Replay's native DXGI and byte-size tables. It handles 1D, 2D, 3D, cube, array and cube-array images, including BC1â€“BC7 variants, exact mip counts and premultiplied-alpha metadata. Replay stores each mip's slices together with 16-byte alignment; export removes this padding and places each slice's mip chain together. Dimensions, allocation extents and read/output budgets are validated before payload reads.

The output uses Microsoft's [DDS header](https://learn.microsoft.com/en-us/windows/win32/direct3ddds/dds-header) and [DX10 extension](https://learn.microsoft.com/en-us/windows/win32/direct3ddds/dds-header-dxt10). Native layout evidence is recorded under the Replay profile's `image_layout`: conversion tables at RVAs `0x23E25F0`/`0x23E4140`, aligned mip size at `0x193BA50`, and subresource offset at `0x193BAE0`.

**Replay clears nonstreamed `GfxImage.pixels` after GPU upload**, confirmed by the native instruction at RVA `0x193888D`. Those images require retained CPU bytes, such as a loader capture, and report `image_uploaded` when unavailable. Fastfile capture/GPU readback remain unfinished. The game-test console image layout is not treated as the PC layout.

For streamed images, supply the loose XPak archives that contain the selected image's keys:

```powershell
acts.exe --noUpdater mw19pools --test image --limit-per-pool 1 --xpak "path/to/archive.xpak" --oodle "C:/absolute/path/oo2core_7_win64.dll" -o output/mw19-image-test
```

Repeat `--xpak` for additional files; later explicit paths take priority for duplicate keys. The reader allows at most 64 archives and 128 MiB of combined in-memory indexes. Oodle is loaded only from an explicitly supplied absolute DLL path. LZ4 and raw entries need no Oodle library. Missing packages, keys or codecs produce unavailable payload statuses; malformed layouts or compressed data fail explicitly. Successful `package_reads` records include the key, source archive, byte count and CRC32. The report-only mode still writes exactly three report files.

Replay reads `streamedPartCount` at byte `0x32`, unlike game-test's `0x31`; the intervening byte retains an unknown label. Native routines at RVAs `0x139F7A0`, `0x13A3280` and `0x13B32F0` establish each part's exclusive size and placement at `totalSize - cumulativeSize`. The exporter validates all part counts, dimensions, mip ranges and byte extents before requesting package data, assembles the complete mip chain, and converts it to DDS. It does not interpret the runtime stream handle as a CPU pointer. Field inspection continues to record that handle as external, so an asset can have a successful DDS payload check while its overall field coverage remains partial.

## Bounded XPak retrieval tests

`mw19xpaktest` reads one explicitly selected entry from a local Replay version 13 XPak, decompresses it in memory, and writes one JSON report containing lengths, CRC32 and metadata. It never writes the extracted payload. A recorded `size0` with `offset0: 0` supplies the output size for a single payload; otherwise supply `--size`.

```powershell
acts.exe --noUpdater mw19xpaktest "path/to/archive.xpak" 0x12345678 --oodle "C:/absolute/path/oo2core_7_win64.dll" -o output/xpak-test.json
```

The reusable `mw19_xpak` reader validates archive version, file/section bounds, sorted index entries, metadata lengths, block sizes and complete output coverage. Replay's command stream uses 128-byte headers, at most 30 commands per header, four-byte block alignment, and a maximum decoded chunk of `0x3FFE0` bytes. It supports raw blocks, LZ4 types 4/5, Oodle types 6/7 and skip commands. Gaps, overlapping output ranges and unsupported codecs fail explicitly. The source console decoder's zlib commands are not assumed to work in Replay PC; native routine RVA `0x13AA780` selects LZ4 and Oodle.

The archive API retrieves a known streamed entry by key and expected size and is integrated with image, model shared-buffer and stream-key export. CASC-backed package discovery remains pending; the current reader opens loose XPak files.

## Model shared buffers and stream keys

`xmodelsurfs` exports the exact `XSurfaceShared` buffer as `.shared.bin`; `streamkey` exports its exact data buffer as `.stream.bin`. Both read resident bytes or retrieve a known XPak key without resolving a runtime handle. Model vertices, indices and other packed attributes remain byte-for-byte intact in the shared buffer. The accompanying header and field outputs retain surface metadata and auxiliary structures. Complete XModel conversion, skeleton and material export remain unfinished.

Add `--geometry` to test or export an additional `.geometry.glb` view of the base surfaces:

```powershell
acts.exe --noUpdater mw19pools --test --geometry xmodelsurfs --limit-per-pool 1 -o output/mw19-geometry-test
```

This uses the already retrieved shared buffer, including an XPak entry when supplied, and does not decode it a second time. In `--test` mode the GLB stays in memory; the three report files gain `geometry_status`, surface/vertex/triangle counts, length and CRC32. An invalid triangle can fail the geometry stage while exact buffer extraction still succeeds. Missing surface headers, mismatched shared-buffer ownership and empty geometry are reported explicitly. The exporter limits a surface set to 4,096 records and checks input, expanded output, attribute extents, finite values and each triangle index.

Replay native instructions at `0x1BB743D`, `0x1BB7397` and `0x1BB74B1` confirm the 192-byte surface record, 20-byte packed vertex and six-byte triangle. Position decoder `0x1992D20` uses three 21-bit coordinates and the largest bounds half-size for all three axes. The normal quaternion and half-float UV layout use the game-test scalar decoder and the matching April 2020 [Greyhound reader](https://github.com/Scobalula/Greyhound/blob/aa0ab50a12f040ef8fb9d00df573cfda16660e0e/src/WraithXCOD/WraithXCOD/GameModernWarfare4.cpp); their evidence is recorded separately from native position/topology verification. The implementation reads versioned format facts independently and does not import third-party exporter code.

The GLB follows [glTF 2.0](https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html), with a root transform from Z-up inches to Y-up meters. It preserves base surface geometry and labels its scope in `extras`; skinning, skeletons, materials, morphs and subdivision are not represented by this view. Packed attributes and auxiliary data remain available through the original shared buffer and structured field outputs. Tangents are not inferred from an unverified handedness convention.

Run `scripts/mw19/test_schema.ps1` for geometry decoding and corruption checks, then `scripts/mw19/test_geometry.ps1` for the real CLI against its own synthetic process. The latter tests one valid triangle and one invalid-index case, independently parses a GLB in memory, compares its checksum with the CLI report and asserts that only report files exist. The optional `scripts/mw19/validate_geometry.cjs` passes the in-memory fixture to Khronos's validator; its header documents the pinned test-only dependency installation under `build`. The current fixture passes with zero errors and warnings; an informational unused-UV message is expected because this geometry view has no material. These are synthetic geometry tests, not live-game model proof.

Native routine `0x13AD700` binds the model XPak info at `+16` and stream-key XPak info at `+8`. `0x13B3390` reads model `shared` at `+48`, shared `dataSize` at `+8`, and stream-key `dataSize` at `+56`. Loaders `0xE2AFA0` and `0xE34050` establish the respective residency flags. Null nonempty resident buffers, missing shared headers, missing keys and unavailable codecs are reported explicitly. The exporter checks combined header/payload read limits and exact decoded lengths.

The stream-key behavior union selects its hash view when the behavior index is zero. Native release routine `0x13ACBB0` confirms that it clears the behavior index and this word together. Active behavior contexts remain unresolved until their runtime types are verified; a successful data-buffer check does not hide that field limitation.

## Sound banks and FLAC samples

Both `soundbank` and `soundbanktransient` export their loaded version 10 SAB container as `.sabl`, using `SndBank.streamInfo.loadedStreamKey` at byte 496. The existing StreamKey reader retrieves resident bytes or a supplied XPak entry. The container is retained exactly, including seek tables and auxiliary records. Missing or empty loaded data is explicitly unavailable; a malformed nonempty bank fails validation.

Add `--audio` to export each supported sample as FLAC. With `--test`, only **one sound per sampled bank** is reconstructed in memory, even if the bank contains more sounds:

```powershell
acts.exe --noUpdater mw19pools --test --audio soundbank soundbanktransient --limit-per-pool 1 -o output/mw19-sound-test
```

Reports include `audio_total_samples`, the sampled keys, sample rate/channels/frame count, loop flag, lengths, CRC32 and separate sample errors. An unsupported codec or corrupt FLAC does not hide a successful exact bank-container extraction. Outside test mode, sample filenames use their numeric asset key and owning bank entry index to avoid name collisions; aliases and original filenames remain in the structured bank data.

For one explicit sound in a loose `.sabs` or `.sabl` file, the separate command reads the header/index and only that sound's encoded bytes, then writes one JSON report:

```powershell
acts.exe --noUpdater mw19soundtest "path/to/bank.sabs" 0x12345678 -o output/mw19-sound-sample.json
```

The reader validates the 688-byte header, version 10/build 16, sorted unique 44-byte index records, file/section/sample extents, metadata overlaps and read/output budgets. Native `0x1B85AB0` validates the header; `0x1B859C0` searches the index; `0xE39F55`/`0xE39F8B` bind the bank's StreamKey; `0x1B86070` loads the resident/streamed bank and locates its index. These bindings are included in the native verifier.

Codec 8 standalone FLAC frames receive a standard [FLAC STREAMINFO block](https://www.rfc-editor.org/rfc/rfc9639.html#section-8.2). The implementation derives block bounds and bit depth from the frame headers, validates fixed/variable frame progression, sample counts, header CRC8 and frame CRC16, and retains the encoded frames unchanged. The PCM MD5 remains unknown, as allowed by STREAMINFO. This validates framing; it is not a full entropy decoder. Unsupported codecs, hybrid PCM samples and a first frame whose bit depth cannot be recovered remain explicitly unavailable. Loop metadata stays in the bank/report; no loop points are invented in the FLAC file. Automatic association of external streamed banks with process aliases remains unfinished.

The tests decoded one real Replay sound from `patch_watch_pet_mean.sabs`, key `0x35ac3fc3`: 26,741 encoded bytes became a 26,783-byte FLAC, CRC32 `715f56a7`. Independent libsndfile decoding produced 26,880 mono frames at 44,100 Hz, 53,760 PCM bytes, CRC32 `373224a3`. Only that sample was read; no FLAC or PCM output file was written. This proves the selected loose-bank sample, not every codec or live-game bank state.

The sound regression suite tests resident and XPak-backed bank containers, fixed/variable and short final FLAC frames, incorrect sample counts/rates, corrupt CRCs, discontinuous frames, bank metadata/extent errors, budgets and unavailable codecs. `test_sound.ps1` also uses a synthetic process bank with two indexed sounds to prove that `--test --audio` touches only one. Its negative fixtures distinguish unsupported codec, corrupt stream and absent bank data. The independent decoder is a test-only dependency:

```powershell
python -m pip install --target build/python soundfile==0.14.0
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/mw19/test_sound.ps1
```

To repeat the bounded real check, add `-Archive "absolute/path/to/patch_watch_pet_mean.sabs" -Key 0x35ac3fc3`. The synthetic process portion still uses generated data. All process test output directories contain only the three report files.

## Validation

Build the normal Windows `AtianCodToolsCLI` target with CMake. The standalone suite uses LLVM `clang-cl` and the release zlib library produced by that build:

```powershell
python scripts/mw19/test_generator.py
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/mw19/test_schema.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/mw19/test_process.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/mw19/test_xpak.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/mw19/test_stream.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/mw19/test_stream.ps1 -Pool xmodelsurfs
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/mw19/test_stream.ps1 -Pool streamkey
```

The schema suite checks all 112 nonzero Replay roots with synthetic empty payloads, the packed allocation stride and MSB-first bitmap, changing allocation flags, invalid types, version-specific offsets, array traversal, malformed compression, shader program offsets, and read/path bounds. It also exercises all 44 scriptable event cases, four state cases, 62 particle module cases and both nine-case physics rule unions. Populated fixtures check selected sound strings, resident image/stream-key bytes, streamed handles, animation tails at the 256-frame boundary, parallel streaming count tables, path-tree branches/leaves and script-bundle words. The image suite also checks all 46 pixel-format byte sizes, array/mip reordering, cube arrays, volume/1D metadata, partial BC blocks, invalid dimensions, short allocations and absent/streamed pixels. Empty-root tests check structural consistency; they are not proof of real asset contents.

The process suite starts a small synthetic executable containing no game code, calls the actual `acts.exe` Windows process reader with its explicit PID, and validates **22 successful assets across all 19 payload-export pools**. This includes compressed and uncompressed rawfiles, a simple RGBA image, an image array with multiple mips, a resident shared mesh buffer, both resident stream-key flags, and both sound-bank classes with in-memory FLAC sample checks. A second bounded run checks missing CPU image pixels and a malformed atlas pointer: missing data is partial, the corrupt field is failed, and the valid DDS payload is still checked independently. An independent Python check compares every payload length and CRC against fixed expected bytes and verifies that the output contains only the three report files. The fixture is stopped after the test. It does not launch Replay.

The stream suite checks three synthetic XPak parts through the actual CLI, including mip assembly, padding removal, the corrected native stream-count offset and explicit unavailable status without package input. The expected DDS is 488 bytes with CRC32 `842ef289`. Standalone cases also reject malformed counts, cumulative sizes, dimensions, incomplete mip chains and short decoded parts before producing a DDS.

A bounded real-package test independently decoded one Oodle entry (`0x2bb083f8e7ef`) from `pak_launch_tu22.xpak`: 38,016 stored bytes became 47,520 decoded bytes, CRC32 `11e94afc`. Using a synthetic process header populated from that entry's metadata, the full pool command produced the expected 47,668-byte DDS in memory, CRC32 `0cb5051b`. An independent Python/Oodle check verified both results. Only JSON/journal reports were written; this proves retrieval and serialization for that entry, not live-game image header capture or all archive entries. To repeat with an existing successful `mw19xpaktest` report for a one-mip BC1 sRGB image:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/mw19/test_stream.ps1 -RealReport "C:/absolute/path/xpak-test.json" -Oodle "C:/absolute/path/oo2core_7_win64.dll"
```

The same bounded process test also passed for one real mesh entry (`0x35754876b3ca51`, 9,136 decoded bytes, CRC32 `edfef29f`) and one real stream-tree entry (`0xf11b581a1c7f69ed`, 48 bytes, CRC32 `5619b9ef`) from that package. Independent decoding matched the pool-command output. Use `-Pool xmodelsurfs` or `-Pool streamkey` with the corresponding successful `mw19xpaktest` report to repeat those checks. Their process headers are synthetic metadata fixtures; the tests do not establish live model geometry or active runtime behavior coverage.

The on-disk native catalog can be checked separately:

```powershell
python scripts/mw19/verify_replay.py "path/to/game_dx12_ship_replay.exe"
```

Verified executable SHA-256: `68fb1cbcb2924182724004039de55a4c50152bb6561803c4898b7930b38132f0`.

A prior live inventory read more than 150,000 asset names across 96 types before Replay exited. That run is incomplete and is not a live payload-export test. No full asset payload dump was performed.

## IW8 fastfile linking

The `fastfilelinker` command now registers `IW8` and `MW19` for **Replay PC xfile version `0xff7`**. See [linker inputs and limits](linker.md). It currently links eight concrete asset types: rawfile, luafile, ttf, localize, scriptfile, stringtable, netconststrings and soundbanklist. Stored and LZ4 IWC containers are supported. Other asset classes fail explicitly; this is not all-pool fastfile round-trip coverage.

`test_all_pools.ps1` checks one synthetic record in each of the 112 nonzero pool roots through the actual process reader and encoder, producing only three report files. All 112 passed: 93 structured exports and 19 conventional exports. Empty root records establish breadth, not real-content coverage.

`test_linker.ps1` builds eight small synthetic assets into unsigned Replay fastfiles. An independent Python reader checks IWC framing, multiple stored/LZ4 blocks, every synthetic asset value, pointer markers, column-major stringtable indexing and exact memory reservations. The existing ACTS decompressor also reproduces the linked body. No game is launched and no game assets are extracted by these tests. Successful serialization/decompression does not establish game acceptance of an unsigned file.

## Layout provenance

Pool IDs, tables, entry layout and allocator behavior come from Replay executable inspection. The game-test type library supplies names and a nested type graph. `scripts/mw19/replay_layouts.py` records explicit Replay substitutions and checks native root offsets after propagating the different GPU record sizes. This accounts for the shader stages, XModel runtime word, removed attachment field, one streaming flag set, reduced client-effects record, world draw differences and larger terrain records.

The checked-in schema builds without proprietary research inputs. Regeneration scripts consume local IDA/type and SQLite exports kept under ignored `research/mw19` paths. `recover_native_loaders.py` obtains call targets from the executable and checks loader byte counts; it does not trust transferred SQLite function names. `generate_pointer_rules.py` accepts only reducible count expressions and rejects conflicting or unresolved formulas. Array loader byte strides take precedence over inaccurate decompiler allocator return types. `union_rules.py` binds switch cases to the actual selected structure and enum value; ambiguous cases stop regeneration. Native Replay routines at RVAs `0xE2FFF0` and `0xE2AFA0` additionally confirm the image and model-surface resident/streamed selectors and byte lengths.

The upstream baseline is `ate47/atian-cod-tools` commit `d6154326b6c95e92843918c4a1ebd19f5ac7c1d7`. The inspected `ProjectHiNAtyu` fork at `8491f89bbfde824a93a1128a50fbac6c801592d2` differs from its common upstream base only in the GitHub build workflow; it supplies no additional MW2019 implementation changes.
