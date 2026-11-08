# Replay parser validation

The September 8, 2026 repair was checked against the owner's Replay executable, SHA-256 `68fb1cbcb2924182724004039de55a4c50152bb6561803c4898b7930b38132f0`, the installed fastfiles and their matching patches. Tests use the executable's serialization routines without starting the game.

## Corrected populated records

| Record / operation | Measured behavior and correction |
| --- | --- |
| Resident `GfxImage` pixels | `Load_GfxImagePixels` calls RVA `0x19387d0` before temporary stream 1 rewinds. Retain the byte buffer at that callback, update the pointer, and account for it in the retained-payload budget. This fixes overwritten DDS data such as `button_alt1`. |
| `DDLMember` | The native array loader shifts the count by six at `0xdf30c3`: stride 64. Its pointer at offset 16 is a serialized byte, not a string. Value fields start at offset 24. |
| `PlayerAnimsetState` | RVA `0xdd1cad` loads 64 bytes. The condition count is at 53 and pointer at 56; condition records occupy 48 bytes. Unnamed condition-mask bytes remain serialized data. |
| `GfxVoxelTree` | RVA `0xd92c2a` multiplies the count by 112. The tail contains runtime state, not the next tree's CPU pointers. |
| `GfxSurfaceBounds` | `Load_GfxWorldSurfaces` selects the pointer at offset 48 at RVA `0xe306c5` and multiplies the count by 56 at `0xe306ff`. Replay overrides the game-test 24-byte record with a 56-byte record. The first 24 bytes retain the midpoint and half-size; the additional 32 bytes are preserved as `serializedExtra` without inventing scalar meanings. Counted arrays expose their resolved `stride` in both expanded and compact exports. |
| Empty shader declaration | Null program plus zero program length exports a descriptor; missing bytes for a nonzero length still fail as unavailable. |
| Manifest replacement | Windows replacement retries temporary access/sharing/lock conflicts, with a bounded wait and an explicit error code on persistent failure. |

`verify_fastfile_bindings.py` checks the 111 loader bindings and nine populated-layout instruction sites before reporting success. The source-derived schema alone is not treated as proof of a Replay layout.

## x64-zt loader-verified layouts (November 8, 2026)

x64-zt (`IDARETW/x64-zt`, branch `iw8-conversion`, commit `caf5b0f`) keeps Replay 1.20 layout corrections in `tools/iw8_layouts.txt`. Each record cites the Replay loader that reads it. Thirty-four records (GPU buffer records, material techniques and pipeline states, world draw vertices and subdivision, terrain meshes, transient costs, Mayhem frames, player animation entries, attachment projectiles, music assets) are ported by `scripts/mw19/replay_layouts_x64zt_{gpu,material,geometry,misc}.py`. They are applied inside `replay_layouts.apply()` before embedded-record propagation, so containers are re-laid out and the existing native root assertions still hold. The rules are:

- An x64-zt record named `Replay::X` replaces `X`, as in x64-zt.
- Runtime-only records that ACTS keeps opaque (`VoxelRuntimeState`, `SurfaceBoundsExtra`, `XModelRuntimeWord`, `PlayerConditionMask`) stay opaque when x64-zt agrees on their size.
- GPU resource pointers and `MaterialPipelineState.pso` carry `runtime` pointer rules.
- `MaterialTechnique` gets Replay pointer counts from `Load_MaterialTechnique` (RVA `0xE0CED0`, `scripts/mw19/replay_rules_x64zt.py`):
  - `pipelineStates`: `pipelineStateCount × pipelineStateInputLayoutCount` records of 16 bytes.
  - `rootParameterMap`: 16 bytes.
  - `stateBits`: `pipelineStateCount` records of 5 bytes.

| Record | Correction (evidence) |
| --- | --- |
| `GfxShaderBufferView` | 16 bytes, not 24: `R_AssignNullShaderBufferView` (RVA `0x18BCA00`) writes `{0, 1}`. Containers such as `XSurfaceSubdivLevel` (568 bytes) shrink or grow accordingly. |
| `GfxWrappedBaseBuffer` / `GfxWrappedBuffer` / `GfxWrappedRWBuffer` / `GfxIndexBuffer` | 48 / 64 / 96 / 32 bytes, from the sizes `Load_GfxWrapped*Buffer` and `Load_GfxIndexBuffer` load. |
| `WeaponAttachment` | The Replay root drops the game-test `hyperBurstInfo`, not `weaponOffsetPatternScaleInfo`. `Load_WeaponAttachment` (RVA `0xE0E480`) passes `+0x238` to `Load_BallisticInfo` (`0xE1DA30`), `+0x240` to the sway/rumble-graph loader (`0xE1F6F0`), and `+0x248` to `Load_WeaponOffsetPatternScaleInfo` (`0xE20BC0`). The call sites are at `0x140E0F792` to `0x140E0F82A`. |
| `MaterialTechnique` | 184 bytes. The serialized members are the name, `pipelineStates` (+0x28), `rootParameterMap` (+0x50), four stage shaders (+0x58 to +0x70), `stateBits` (+0x88), `defaultStateBits` (+0xA0, in place) and `args` (+0xB0). |

`scripts/mw19/check_replay_layouts.py` rebuilds the schema in memory and checks three things:

1. Every x64-zt record matches the final ACTS type: size and member offsets for records, offset and type for patches.
2. Every struct is self-consistent.
3. Every pointer rule binds as `mw19_schema.cpp` binds it: literal names at load, override-resolved count paths at traversal.

Result: 34 of 34 records match, 1,038 structs are consistent, there are 0 rule problems, and there are 67 type overrides.

**Stock-zone cross-check.** `zonetool -game iw8 verifyzone` (x64-zt `caf5b0f`, the same layouts) consumed every serialized byte of 76 stock Replay 1.20 zones. The three frontend zones of another Replay version were excluded. Its asset trace covers 102 of the 117 pools. `generate_schema.py` records the result per pool as `x64zt_crosscheck`, with zone and asset counts, from the local evidence file `research/mw19/x64zt_verifyzone_coverage.json`. Pools with only a game-test size match move from `source_size_match` to `replay_stream_crosscheck`. The 14 `native_pointer_layout` pools keep that status. **A/B on stock zones.** The same `acts.exe` build ran with the previous schema (HEAD) and with this one (`scripts/mw19/compare_replay_runs.py`). Results:

- **`--test` on the four zones.** `mp_shipment`, `mp_hideout`, `mp_backlot2` and `common_mp` gave identical loaded, tested, failed and unavailable counts. `common_mp` keeps its two pre-existing `streamkey`/`soundbank` failures. Read errors and unresolved pointers or unions did not increase anywhere (0 regressions). Field counts change only where layouts changed; for example, `stterrain` goes from 15 to 26 nodes with typed terrain meshes.
- **`mp_shipment`, every asset of the touched types.** `gfx_map`, `gfx_map_trzone`, `stterrain` and `aianimset` are all `ok` with 0 errors. `xmodelsurfs` has 202 `ok` and 1,127 unavailable, the same as before; their payloads are only in XPaks.
- **`code_post_gfx`, every asset of the touched types.** 371 materials, 339 techsets, the attachment, Mayhem and camo assets are all `ok` with 0 errors in both runs. Techsets decode 87,944 field nodes, up from 41,862, now that the Replay `MaterialTechnique` exposes its pipeline states and state bits.

Ten pools still have only `source_size_match`: nine never occur in the scanned zones (`physicsdebugdata`, `playeranim`, `scriptdebugdata`, `addon_map_ents`, `spaceshiptarget`, `scriptbundle`, `streamtreeoverride`, `nativescriptpatch`, `soundbanklist`), and `iesprofile` has no reader in the IW8 format table. Exact consumption proves the stream extents, not every scalar meaning.

## Stock world loading and surface bounds

The reported failures concerned `mp_frontend3.ff`, `mp_rust.ff`, and `mp_hideout.ff`. Their matching adjacent patches had been omitted from the reader invocation. Applying those patches produces final revision `0xff7` and resolves the packed-pointer and stream-reservation failures. The `mw19replay` reader now automatically discovers `.fp` followed by `.fc`; each patch must still match its actual previous header. `--no-auto-patch` permits deliberate base-file inspection. Manifests record the input revision, effective revision, and applied patch paths separately.

The rebuilt reader passed the following stock checks without explicit patch flags. Each zone completed loading and tested `gfx_map`, `gfx_map_trzone`, and `com_map`, with zero failed or unavailable selected exports. A separate disk pass exported only `gfx_map`; every surface-bounds record was checked for 56-byte stride, matching surface count, finite coordinates, and nonnegative half-size.

| Zone | Loaded records | Input / effective revision | Verified surface bounds |
| --- | ---: | --- | ---: |
| `mp_frontend3.ff` | 2,200 | `0xff5` / `0xff7` | 140 |
| `mp_rust.ff` | 7,215 | `0xfe1` / `0xff7` | 412 |
| `mp_hideout.ff` | 18,642 | `0xff3` / `0xff7` | 3,301 |
| `mp_shipment.ff` | 10,478 | `0xff7` / `0xff7` (no patch) | 1,039 |

Reproduce with `python scripts/mw19/test_stock_worlds.py --game <replay-exe> --zone <zone-directory> --oodle <oodle-dll> --out <new-directory> --write-worlds`. Omit `--write-worlds` for report-only capability checks. The script writes a summary with manifest results, bounds counts, export paths and SHA-256 hashes. These checks validate selected world exporters after complete zone loading; they do not assert that all other asset payloads in these zones are available.

## Older frontend layout

The patched `mp_frontend.ff` uses revision `0xfda`. The adapter handles these differences before current Replay pointer fixups:

- `ComWorld`: 152-byte root, 344-byte primary-light records with the definition-name pointer at offset 336; no current transient-table fields.
- `ScriptableDef`: 112-byte root without the later network-LOD override fields.
- `GfxWorld`: 17,744-byte root with older sort-key and visibility arrays, including 31 visibility views.
- Per-light meshes: 96-byte descriptors, 16-bit indices and 32-byte vertices aligned to four bytes.
- Light view frustums: 48-byte descriptors with three 32-bit counts and 16-bit indices. Conversion checks every count and index before representing these in the current layout.

The original unrelocated world header and light descriptors, including full 32-byte mesh vertices, are retained in `asset.serialized_layout`. Normalized position and index arrays can be inspected using the current field schema. Unnamed serialized scalar records are marked `opaque_serialized_record`; they are not mislabeled as runtime-only data.

This is measured coverage for the exercised layouts, not a complete semantic map of every historical field. In particular, the older dynamic-lightset region is empty in this frontend. A populated region is rejected until its matching layout is mapped. Original bytes are retained for the supported export, rather than silently assigning uncertain fields. Other supported revision IDs still require valid streams and layouts for the assets they contain.

## Selected full extractions

These two owner-selected zones were extracted to disk through the authenticated viewer, with all asset types enabled and geometry sidecars. This did not dump the whole installation.

| Zone | Loaded records | Failed exports | Unavailable payloads | Result |
| --- | ---: | ---: | ---: | --- |
| `code_post_gfx.ff` | 6,012 | 0 | 0 | Complete export, including all 26 DDLs and 305 images. `button_alt1` passes decoded pixel checks. |
| `mp_frontend.ff` | 2,013 | 0 | 109 | All 1,674 top-level entries and all 75,598,161 serialized bytes loaded. External payloads missing from the installed XPaks remain explicit. |

The 109 frontend payloads comprise 60 images, 48 model-surface buffers and one stream-tree key. A separate read-only scan of all 265,738 indexed keys in the 14 populated installed XPaks found none of the four parts for any of those 60 images (240 absent part keys). Lower mips therefore cannot recover those particular images from these archives.

The viewer still prepares exact material dependencies for partial output. For this frontend it indexed 271 surface sets and 214 materials, with zero unresolved material names and 18 unresolved color-image names after checking the installed companion zones. A missing external payload is distinct from a malformed serialized field or a failed exporter.

## Regression coverage

- All 111 enabled native loader bindings; 108 successful empty-root exporters and three explicitly unavailable empty payload types.
- Eight populated linker/exporter round trips, plus malformed stream, count and pointer rejection.
- Two successive resident images with different bytes, proving that later temporary-buffer reuse cannot corrupt the first image.
- Two populated 64-byte DDL members, including names, value fields and the single-byte pointer.
- Two consecutive 56-byte surface-bounds records through native serialization and structured export, including all 32 trailing bytes; a separate C++ fixture checks the same stride and preservation.
- Automatic patch discovery, patch-state reset between files, base-only opt-out, explicit patch override, ordered `.fp`/`.fc` chaining, and rejection of mismatched or empty patches.
- Synthetic older frontend world, light mesh, view frustum and scriptable records, plus a subsequent final-revision file in the same process to check loader restoration.
- C++ tests for populated player condition records and successive 112-byte voxel trees; a Windows reader denying replacement briefly during an atomic manifest update.
- Browser checks of the actual exported button image and frontend texture assignments on desktop and mobile, with the mobile checks also passing through the external HTTPS tunnel. The care package has 6/6 surfaces textured, the deployable cover 11/11, and the perk patch 1/1 before and after changing its material variant.
- Python regressions for material preparation after partial extraction and cancellation arriving immediately after a successful child exit. The earlier viewer repair passed 20 Python tests; the current ACTS native fastfile suite passed all 53 command runs and its payload assertions.

Empty-root checks establish dispatch and structural consistency. They do not establish every populated field, optional codec, game-renderer shader behavior or all historical fastfiles. Native output, structured field inspection, decoded image pixels and browser rendering are validated separately.

## GSC source preview

Exported `.gscbin` files can be decompiled separately with:

```powershell
acts.exe --noUpdater gscd -g -v iw8 -f iw --path-output -o output/gsc script.gscbin
python scripts/mw19/test_gsc_preview.py --out build/mw19-tests/gsc-local-calls
```

IW8 stores a signed 24-bit local-function displacement with one low tag bit. The GSCBIN reader now removes that bit for IW8 before resolving local targets, including function references, function/method calls and threaded calls. It reads exactly three operand bytes instead of dereferencing a four-byte word at the end of a buffer. Other VMs retain their existing displacement convention. This agrees with the local IW8 disassembler's `offs9` convention and the selected Replay script: the encoded operand `0xfffb30` at bytecode offset `0x269` resolves to function offset `1`.

The synthetic regression verifies both forward and backward local calls. The selected extracted Shipment script `1229` also decompiled without the previous `<errlocal:...>` marker. Missing symbol dictionaries still produce numeric names; other decompiler limitations remain explicit. The viewer runs this command on demand for a selected script, caches the result, highlights the reconstructed source and retains the original compiled download.
