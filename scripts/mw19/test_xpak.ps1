$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$dest = Join-Path $root 'build/mw19-tests'
New-Item -ItemType Directory -Force -Path $dest | Out-Null
Push-Location $dest
try {
    & 'C:/Program Files/LLVM/bin/clang-cl.exe' /nologo /MD /EHsc /std:c++20 /DMW19_SCHEMA_STANDALONE `
        "/I$root/src/core/acts" "/I$root/deps/json/include" "/I$root/deps/lz4/lib" `
        "$root/scripts/mw19/xpak_test.cpp" "$root/src/core/acts/tools/mw19/mw19_xpak.cpp" `
        "$root/build/bin/Release/lz4.lib" /Fe:mw19-xpak-test.exe
    if ($LASTEXITCODE -ne 0) { throw 'XPak fixture compile failed' }
    & ./mw19-xpak-test.exe
    if ($LASTEXITCODE -ne 0) { throw 'XPak fixture failed' }
} finally { Pop-Location }
