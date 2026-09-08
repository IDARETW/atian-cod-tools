param([string]$Archive = '', [string]$Key = '0x1234')
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$dest = Join-Path $root ('build/mw19-tests/sound-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Force -Path $dest | Out-Null
$synthetic = !$Archive
if (!$Archive) {
    $Archive = Join-Path $dest 'synthetic.sabs'
    & python "$PSScriptRoot/verify_sound_report.py" create $Archive
    if ($LASTEXITCODE -ne 0) { throw 'Cannot create synthetic bank input' }
}
$report = Join-Path $dest 'sample-report.json'
& "$root/build/bin/Release/acts.exe" --noUpdater mw19soundtest $Archive $Key -o $report
if ($LASTEXITCODE -ne 0) { throw 'SAB/FLAC sample capability test failed' }
& python "$PSScriptRoot/verify_sound_report.py" verify $report --executable "$root/build/mw19-tests/mw19-sound-test.exe"
if ($LASTEXITCODE -ne 0) { throw 'Independent sound decoding failed' }
if ((Get-ChildItem -LiteralPath $dest -Filter '*.flac').Count) { throw 'Test wrote an audio asset' }
Write-Output "Sound sample report: $report"
if ($synthetic) {
    $before = (Get-FileHash -LiteralPath $Archive -Algorithm SHA256).Hash
    & "$root/build/bin/Release/acts.exe" --noUpdater mw19soundtest $Archive $Key -o $Archive
    if ($LASTEXITCODE -eq 0 -or (Get-FileHash -LiteralPath $Archive -Algorithm SHA256).Hash -ne $before) {
        throw 'Sound report overwrote its synthetic input or incorrectly succeeded'
    }
}
# The process fixture uses synthetic data even when a real archive was selected above.
$schema = Join-Path $root 'config/data/mw19/schema.json'
$ready = Join-Path $dest 'ready.json'
$stop = Join-Path $dest 'stop.txt'
$spec = Join-Path $dest 'fixture.json'
Set-Content -LiteralPath $spec -Value '{"pool":"soundbank","sound_fixture":true}' -Encoding ascii
$fixture = $null
Push-Location $dest
try {
    & 'C:\Program Files\LLVM\bin\clang-cl.exe' /nologo /MD /EHsc /std:c++20 `
        "/I$root/deps/json/include" "/I$root/deps/zlib" "$root/scripts/mw19/process_fixture.cpp" `
        "$root/build/bin/Release/zlib.lib" /Fe:game_dx12_ship_replay.exe /link /BASE:0x140000000 /DYNAMICBASE:NO
    if ($LASTEXITCODE -ne 0) { throw 'Sound process fixture compile failed' }
    $arguments = @($schema, $ready, $stop, $spec) | ForEach-Object { '"' + $_ + '"' }
    $fixture = Start-Process -FilePath (Join-Path $dest 'game_dx12_ship_replay.exe') -ArgumentList $arguments `
        -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $dest 'fixture.stdout.log') `
        -RedirectStandardError (Join-Path $dest 'fixture.stderr.log')
    $deadline = (Get-Date).AddSeconds(15)
    while (!(Test-Path -LiteralPath $ready)) {
        if ($fixture.HasExited -or (Get-Date) -gt $deadline) { throw 'Sound fixture failed to start' }
        Start-Sleep -Milliseconds 100
    }
    $report = Join-Path $dest 'report'
    & "$root/build/bin/Release/acts.exe" --noUpdater mw19pools --pid $fixture.Id --test --audio -o $report soundbank
    if ($LASTEXITCODE -ne 0) { throw 'Sound bank capability test failed' }
    & python "$PSScriptRoot/verify_sound_report.py" verify (Join-Path $report 'manifest.json') --process
    if ($LASTEXITCODE -ne 0) { throw 'Sound bank report verification failed' }
    $report = Join-Path $dest 'negative-report'
    & "$root/build/bin/Release/acts.exe" --noUpdater mw19pools --pid $fixture.Id --test --audio --limit-per-pool 4 -o $report soundbank
    if ($LASTEXITCODE -eq 0) { throw 'Unsupported, corrupt or missing sound samples incorrectly succeeded' }
    & python "$PSScriptRoot/verify_sound_report.py" verify (Join-Path $report 'manifest.json') --process --negative
    if ($LASTEXITCODE -ne 0) { throw 'Sound failure reporting test failed' }
} finally {
    Set-Content -LiteralPath $stop -Value 'stop'
    if ($fixture -and !$fixture.HasExited) {
        if (!$fixture.WaitForExit(5000)) { Stop-Process -Id $fixture.Id }
    }
    Pop-Location
}
