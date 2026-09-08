$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$dest = Join-Path $root ('build/mw19-tests/process-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Force -Path $dest | Out-Null
$compiler = 'C:\Program Files\LLVM\bin\clang-cl.exe'
$schema = Join-Path $root 'config/data/mw19/schema.json'
$ready = Join-Path $dest 'ready.json'
$stop = Join-Path $dest 'stop.txt'
$report = Join-Path $dest 'report'
Push-Location $dest
$fixture = $null
try {
    & $compiler /nologo /MD /EHsc /std:c++20 "/I$root/deps/json/include" "/I$root/deps/zlib" `
        "$root/scripts/mw19/process_fixture.cpp" "$root/build/bin/Release/zlib.lib" `
        /Fe:game_dx12_ship_replay.exe /link /BASE:0x140000000 /DYNAMICBASE:NO
    if ($LASTEXITCODE -ne 0) { throw 'Fixture compile failed' }
    $arguments = @($schema, $ready, $stop) | ForEach-Object { '"' + $_ + '"' }
    $fixture = Start-Process -FilePath (Join-Path $dest 'game_dx12_ship_replay.exe') -ArgumentList $arguments `
        -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $dest 'fixture.stdout.log') `
        -RedirectStandardError (Join-Path $dest 'fixture.stderr.log')
    $deadline = (Get-Date).AddSeconds(15)
    while (!(Test-Path -LiteralPath $ready)) {
        if ($fixture.HasExited -or (Get-Date) -gt $deadline) { throw 'Fixture failed to become ready' }
        Start-Sleep -Milliseconds 100
    }
    & "$root/build/bin/Release/acts.exe" --noUpdater mw19pools --pid $fixture.Id --test --audio --limit-per-pool 2 -o $report `
        rawfile scriptfile luafile stringtable localize netconststrings ttf soundbanklist computeshader libshader vertexshader hullshader domainshader pixelshader image streamkey xmodelsurfs soundbank soundbanktransient
    $resultCode = $LASTEXITCODE
    $manifest = Get-Content -Raw -LiteralPath (Join-Path $report 'manifest.json') | ConvertFrom-Json
    if (!$manifest.complete -or $manifest.assets.Count -ne 22 -or $manifest.failures -ne 0) { throw 'Capability fixture failed; inspect report' }
    if ($resultCode -ne 0 -or $manifest.partials -ne 0) { throw 'Capability fixture has partial exports; inspect report' }
    if (Test-Path -LiteralPath (Join-Path $report 'assets')) { throw 'Report-only test wrote asset files' }
    $formats = @($manifest.assets | Select-Object -ExpandProperty payload_format -Unique)
    if ($formats.Count -ne 13) { throw "Expected thirteen payload formats, got $($formats.Count)" }
    & python "$root/scripts/mw19/verify_test_report.py" $report
    if ($LASTEXITCODE -ne 0) { throw 'Fixture payload verification failed' }
    $manifest.assets | Format-Table pool,status,payload_bytes,payload_status -AutoSize
    Write-Output "22 synthetic assets in 19 pools tested through the Windows process reader; asset output directory absent. CLI exit: $resultCode. Report: $report"
    $negativeReport = Join-Path $dest 'negative-report'
    & "$root/build/bin/Release/acts.exe" --noUpdater mw19pools --pid $fixture.Id --test --limit-per-pool 4 -o $negativeReport image
    if ($LASTEXITCODE -eq 0) { throw 'Incomplete image capabilities incorrectly reported success' }
    & python "$root/scripts/mw19/verify_test_report.py" $negativeReport --negative
    if ($LASTEXITCODE -ne 0) { throw 'Failure reporting or independent stage verification failed' }
} finally {
    Set-Content -LiteralPath $stop -Value 'stop'
    if ($fixture -and !$fixture.HasExited) {
        if (!$fixture.WaitForExit(5000)) { Stop-Process -Id $fixture.Id }
    }
    Pop-Location
}
