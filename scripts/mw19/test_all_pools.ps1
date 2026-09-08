$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$dest = Join-Path $root ('build/mw19-tests/all-pools-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Force -Path $dest | Out-Null
$schema = Join-Path $root 'config/data/mw19/schema.json'
$ready = Join-Path $dest 'ready.json'
$stop = Join-Path $dest 'stop.txt'
$spec = Join-Path $dest 'fixture.json'
Set-Content -LiteralPath $spec -Value '{"schema_fixture":true}' -Encoding ascii
$fixture = $null
Push-Location $dest
try {
    & 'C:\Program Files\LLVM\bin\clang-cl.exe' /nologo /MD /EHsc /std:c++20 "/I$root/deps/json/include" "/I$root/deps/zlib" `
        "$root/scripts/mw19/process_fixture.cpp" "$root/build/bin/Release/zlib.lib" `
        /Fe:game_dx12_ship_replay.exe /link /BASE:0x140000000 /DYNAMICBASE:NO
    if ($LASTEXITCODE -ne 0) { throw 'Field process fixture compile failed' }
    $arguments = @($schema, $ready, $stop, $spec) | ForEach-Object { '"' + $_ + '"' }
    $fixture = Start-Process -FilePath (Join-Path $dest 'game_dx12_ship_replay.exe') -ArgumentList $arguments `
        -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $dest 'fixture.stdout.log') `
        -RedirectStandardError (Join-Path $dest 'fixture.stderr.log')
    $deadline = (Get-Date).AddSeconds(15)
    while (!(Test-Path -LiteralPath $ready)) {
        if ($fixture.HasExited -or (Get-Date) -gt $deadline) { throw 'Field fixture failed to start' }
        Start-Sleep -Milliseconds 100
    }
    $report = Join-Path $dest 'report'
    & "$root/build/bin/Release/acts.exe" --noUpdater mw19pools --pid $fixture.Id --test --limit-per-pool 1 -o $report --all
    if ($LASTEXITCODE -ne 0) { throw 'All-pool fixture failed; inspect report' }
    $manifest = Get-Content -Raw -LiteralPath (Join-Path $report 'manifest.json') | ConvertFrom-Json
    if (!$manifest.complete -or $manifest.assets.Count -ne 112 -or $manifest.partials -ne 0 -or $manifest.failures -ne 0) {
        throw 'All-pool result counts incorrect'
    }
    if (@($manifest.assets | Where-Object { $_.payload_format -eq 'mw19-asset-json-v1' }).Count -ne 93) {
        throw 'Expected 93 structured formats alongside 19 conventional formats'
    }
    $files = @(Get-ChildItem -LiteralPath $report -File -Recurse | Sort-Object Name | Select-Object -ExpandProperty Name)
    if (($files -join ',') -ne 'assets.jsonl,manifest.json,progress.json') { throw 'Test wrote unexpected files' }
    Write-Output "112 synthetic pool roots: 93 structured and 19 conventional exports validated. Empty roots are breadth smoke coverage. Only three report files written: $report"
} finally {
    Set-Content -LiteralPath $stop -Value 'stop'
    if ($fixture -and !$fixture.HasExited) {
        if (!$fixture.WaitForExit(5000)) { Stop-Process -Id $fixture.Id }
    }
    Pop-Location
}
