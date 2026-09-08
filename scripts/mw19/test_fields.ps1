$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$dest = Join-Path $root ('build/mw19-tests/fields-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Force -Path $dest | Out-Null
$schema = Join-Path $root 'config/data/mw19/schema.json'
$ready = Join-Path $dest 'ready.json'
$stop = Join-Path $dest 'stop.txt'
$spec = Join-Path $dest 'fixture.json'
Set-Content -LiteralPath $spec -Value '{"pool":"weapon","field_fixture":true}' -Encoding ascii
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
    & "$root/build/bin/Release/acts.exe" --noUpdater mw19pools --pid $fixture.Id --test --limit-per-pool 2 -o $report weapon
    if ($LASTEXITCODE -eq 0) { throw 'Unreadable weapon curve incorrectly passed' }
    $manifest = Get-Content -Raw -LiteralPath (Join-Path $report 'manifest.json') | ConvertFrom-Json
    if (!$manifest.complete -or $manifest.assets.Count -ne 2 -or $manifest.partials -ne 0 -or $manifest.failures -ne 1) {
        throw 'Unexpected field capability result counts'
    }
    $good = $manifest.assets[0]
    $bad = $manifest.assets[1]
    if ($good.field_status -ne 'ok' -or $good.field_issue_count -ne 0 -or $good.header_status -ne 'ok') {
        throw 'Weapon curve arrays were not fully inspected'
    }
    if ($good.payload_status -ne 'ok' -or $good.payload_format -ne 'mw19-asset-json-v1' -or $bad.payload_status -ne 'failed') {
        throw 'Structured export did not reject incomplete fields'
    }
    # vec2_t retains two labelled union views, each containing two scalars.
    if ($bad.field_status -ne 'failed' -or $bad.header_status -ne 'ok' -or $bad.field_issue_count -ne 4 -or
        @($bad.field_issues | Where-Object { !$_.path.StartsWith('fields/accuracyGraphKnots/0/values/0/') }).Count -ne 0) {
        throw 'Unreadable array slot did not retain independent stage status and issue path'
    }
    $files = @(Get-ChildItem -LiteralPath $report -File -Recurse | Sort-Object Name | Select-Object -ExpandProperty Name)
    if (($files -join ',') -ne 'assets.jsonl,manifest.json,progress.json') { throw 'Test wrote unexpected files' }
    Write-Output "Weapon fields: two array extents inspected, corrupt pointer isolated, structured export validated. Only three report files written: $report"
} finally {
    Set-Content -LiteralPath $stop -Value 'stop'
    if ($fixture -and !$fixture.HasExited) {
        if (!$fixture.WaitForExit(5000)) { Stop-Process -Id $fixture.Id }
    }
    Pop-Location
}
