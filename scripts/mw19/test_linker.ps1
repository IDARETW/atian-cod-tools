$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$dest = Join-Path $root ('build/mw19-tests/linker-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Force -Path $dest | Out-Null
Push-Location $dest
try {
    & python "$root/scripts/mw19/linker_fixture.py" $dest
    if ($LASTEXITCODE -ne 0) { throw 'Cannot create synthetic linker inputs' }
    & 'C:\Program Files\LLVM\bin\clang-cl.exe' /nologo /MD /EHsc /std:c++20 /DMW19_SCHEMA_STANDALONE `
        "/I$root/src/core/acts" "/I$root/deps/json/include" "/I$root/deps/zlib" "/I$root/deps/lz4/lib" `
        "$root/scripts/mw19/linker_test.cpp" "$root/src/core/acts/tools/mw19/mw19_linker.cpp" `
        "$root/src/core/acts/tools/mw19/mw19_schema.cpp" "$root/build/bin/Release/zlib.lib" "$root/build/bin/Release/lz4.lib" `
        /Fe:mw19-linker-test.exe
    if ($LASTEXITCODE -ne 0) { throw 'Linker tests compile failed' }
    & ./mw19-linker-test.exe "$root/config/data/mw19/schema.json" "$dest/fixtures.json" $dest
    if ($LASTEXITCODE -ne 0) { throw 'Linker unit tests failed' }
    & python "$root/scripts/mw19/verify_linker.py" $dest "$root/config/data/mw19/schema.json"
    if ($LASTEXITCODE -ne 0) { throw 'Independent linker verification failed' }
    & "$root/build/bin/Release/acts.exe" --noUpdater fastfilelinker -o "$dest/cli" "$dest/synthetic.zone"
    if ($LASTEXITCODE -ne 0) { throw 'Registered IW8 linker failed' }
    & python "$root/scripts/mw19/linker_fixture.py" $dest --verify-cli
    if ($LASTEXITCODE -ne 0) { throw 'CLI output differs from independent fixtures' }
    & "$root/build/bin/Release/acts.exe" --noUpdater fastfile -d -o "$dest/reader" "$dest/cli/zone/synthetic.ff"
    if ($LASTEXITCODE -ne 0) { throw 'ACTS reader rejected synthetic fastfile' }
    if ((Get-FileHash "$dest/expected.body").Hash -ne (Get-FileHash "$dest/reader/synthetic.ff.dec").Hash) {
        throw 'ACTS decompressor round trip differs'
    }
    & "$root/build/bin/Release/acts.exe" --noUpdater fastfilelinker -o "$dest/negative" "$dest/bad.zone"
    if ($LASTEXITCODE -eq 0 -or (Test-Path -LiteralPath "$dest/negative/zone/bad.ff")) { throw 'Invalid asset was silently linked' }
    & "$root/build/bin/Release/acts.exe" --noUpdater fastfilelinker -o "$dest/negative" "$dest/corrupt.zone"
    if ($LASTEXITCODE -eq 0 -or (Test-Path -LiteralPath "$dest/negative/zone/corrupt.ff")) { throw 'Corrupt GSCBIN was silently linked' }
    & "$root/build/bin/Release/acts.exe" --noUpdater fastfilelinker -o "$dest/empty-out" "$dest/empty.zone"
    if ($LASTEXITCODE -ne 0) { throw 'Empty GSCBIN rejected' }
    & python "$root/scripts/mw19/linker_fixture.py" $dest --verify-empty
    if ($LASTEXITCODE -ne 0) { throw 'Empty ScriptFile verification failed' }
    $before = (Get-FileHash "$dest/zone/collision.ff").Hash
    & "$root/build/bin/Release/acts.exe" --noUpdater fastfilelinker -o $dest "$dest/collision.zone"
    if ($LASTEXITCODE -eq 0 -or (Get-FileHash "$dest/zone/collision.ff").Hash -ne $before) { throw 'Linker overwrote an input' }
    Write-Output "IW8 linker synthetic tests passed: $dest"
} finally { Pop-Location }
