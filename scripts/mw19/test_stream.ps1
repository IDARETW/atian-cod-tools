param([string]$RealReport = '', [string]$Oodle = '', [ValidateSet('image', 'streamkey', 'xmodelsurfs')][string]$Pool = 'image')
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$dest = Join-Path $root ('build/mw19-tests/stream-' + $Pool + '-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Force -Path $dest | Out-Null
$createArgs = @('create', $dest, '--pool', $Pool)
if ($RealReport) { $createArgs += @('--real-report', $RealReport) }
& python "$PSScriptRoot/stream_fixture.py" @createArgs
if ($LASTEXITCODE -ne 0) { throw 'Stream fixture input generation failed' }
$spec = Join-Path $dest 'image.json'
$ready = Join-Path $dest 'ready.json'
$stop = Join-Path $dest 'stop.txt'
$schema = Join-Path $root 'config/data/mw19/schema.json'
$compiler = 'C:\Program Files\LLVM\bin\clang-cl.exe'
$fixture = $null
Push-Location $dest
try {
    & $compiler /nologo /MD /EHsc /std:c++20 "/I$root/deps/json/include" "/I$root/deps/zlib" `
        "$root/scripts/mw19/process_fixture.cpp" "$root/build/bin/Release/zlib.lib" `
        /Fe:game_dx12_ship_replay.exe /link /BASE:0x140000000 /DYNAMICBASE:NO
    if ($LASTEXITCODE -ne 0) { throw 'Fixture compile failed' }
    $arguments = @($schema, $ready, $stop, $spec) | ForEach-Object { '"' + $_ + '"' }
    $fixture = Start-Process -FilePath (Join-Path $dest 'game_dx12_ship_replay.exe') -ArgumentList $arguments `
        -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $dest 'fixture.stdout.log') `
        -RedirectStandardError (Join-Path $dest 'fixture.stderr.log')
    $deadline = (Get-Date).AddSeconds(15)
    while (!(Test-Path -LiteralPath $ready)) {
        if ($fixture.HasExited -or (Get-Date) -gt $deadline) { throw 'Fixture did not start' }
        Start-Sleep -Milliseconds 100
    }
    $archive = Join-Path $dest 'synthetic.xpak'
    if ($RealReport) { $archive = (Get-Content -Raw -LiteralPath $RealReport | ConvertFrom-Json).archive }
    $packageArgs = @('--xpak', $archive)
    $verifyArgs = @()
    if ($Oodle) { $packageArgs += @('--oodle', $Oodle); $verifyArgs += @('--oodle', $Oodle) }
    $report = Join-Path $dest 'report'
    & "$root/build/bin/Release/acts.exe" --noUpdater mw19pools --pid $fixture.Id --test -o $report @packageArgs $Pool
    if ($LASTEXITCODE -eq 0) { throw 'External runtime handle was incorrectly classified as fully traversed' }
    & python "$PSScriptRoot/stream_fixture.py" verify $report --spec $spec @verifyArgs
    if ($LASTEXITCODE -ne 0) { throw 'Streamed DDS report verification failed' }
    $missingReport = Join-Path $dest 'missing-report'
    & "$root/build/bin/Release/acts.exe" --noUpdater mw19pools --pid $fixture.Id --test -o $missingReport $Pool
    if ($LASTEXITCODE -eq 0) { throw 'Missing package input incorrectly succeeded' }
    & python "$PSScriptRoot/stream_fixture.py" verify $missingReport --spec $spec --missing
    if ($LASTEXITCODE -ne 0) { throw 'Missing package report verification failed' }
    Write-Output "Stream capability reports: $dest"
} finally {
    Set-Content -LiteralPath $stop -Value 'stop'
    if ($fixture -and !$fixture.HasExited) {
        if (!$fixture.WaitForExit(5000)) { Stop-Process -Id $fixture.Id }
    }
    Pop-Location
}
