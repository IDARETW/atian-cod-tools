$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$dest = Join-Path $root ('build/mw19-tests/geometry-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Force -Path $dest | Out-Null
$schema = Join-Path $root 'config/data/mw19/schema.json'
$ready = Join-Path $dest 'ready.json'
$stop = Join-Path $dest 'stop.txt'
$spec = Join-Path $dest 'fixture.json'
Set-Content -LiteralPath $spec -Value '{"pool":"xmodelsurfs","geometry_fixture":true}' -Encoding ascii
$compiler = 'C:\Program Files\LLVM\bin\clang-cl.exe'
$fixture = $null
Push-Location $dest
try {
    & $compiler /nologo /MD /EHsc /std:c++20 "/I$root/deps/json/include" "/I$root/deps/zlib" `
        "$root/scripts/mw19/process_fixture.cpp" "$root/build/bin/Release/zlib.lib" `
        /Fe:game_dx12_ship_replay.exe /link /BASE:0x140000000 /DYNAMICBASE:NO
    if ($LASTEXITCODE -ne 0) { throw 'Geometry process fixture compile failed' }
    $arguments = @($schema, $ready, $stop, $spec) | ForEach-Object { '"' + $_ + '"' }
    $fixture = Start-Process -FilePath (Join-Path $dest 'game_dx12_ship_replay.exe') -ArgumentList $arguments `
        -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $dest 'fixture.stdout.log') `
        -RedirectStandardError (Join-Path $dest 'fixture.stderr.log')
    $deadline = (Get-Date).AddSeconds(15)
    while (!(Test-Path -LiteralPath $ready)) {
        if ($fixture.HasExited -or (Get-Date) -gt $deadline) { throw 'Geometry fixture failed to start' }
        Start-Sleep -Milliseconds 100
    }
    $report = Join-Path $dest 'report'
    & "$root/build/bin/Release/acts.exe" --noUpdater mw19pools --pid $fixture.Id --test --geometry -o $report xmodelsurfs
    if ($LASTEXITCODE -ne 0) { throw 'Geometry capability test failed' }
    & python "$PSScriptRoot/verify_geometry_report.py" $report "$root/build/mw19-tests/mw19-mesh-test.exe"
    if ($LASTEXITCODE -ne 0) { throw 'Geometry report verification failed' }
    $report = Join-Path $dest 'negative-report'
    & "$root/build/bin/Release/acts.exe" --noUpdater mw19pools --pid $fixture.Id --test --geometry --limit-per-pool 2 -o $report xmodelsurfs
    if ($LASTEXITCODE -eq 0) { throw 'Corrupt triangle indices incorrectly succeeded' }
    & python "$PSScriptRoot/verify_geometry_report.py" $report "$root/build/mw19-tests/mw19-mesh-test.exe" --negative
    if ($LASTEXITCODE -ne 0) { throw 'Geometry failure reporting test failed' }
} finally {
    Set-Content -LiteralPath $stop -Value 'stop'
    if ($fixture -and !$fixture.HasExited) {
        if (!$fixture.WaitForExit(5000)) { Stop-Process -Id $fixture.Id }
    }
    Pop-Location
}
