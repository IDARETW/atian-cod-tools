$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$compiler = 'C:\Program Files\LLVM\bin\clang-cl.exe'
$dest = Join-Path $root 'build/mw19-tests'
New-Item -ItemType Directory -Force -Path $dest | Out-Null
Push-Location $dest
try {
    & $compiler /nologo /MD /EHsc /std:c++20 /DMW19_SCHEMA_STANDALONE `
        "/I$root/src/core/acts" "/I$root/deps/json/include" "/I$root/deps/zlib" `
        "$root/scripts/mw19/schema_test.cpp" "$root/src/core/acts/tools/mw19/mw19_schema.cpp" `
        "$root/src/core/acts/tools/mw19/mw19_payload.cpp" "$root/src/core/acts/tools/mw19/mw19_sound.cpp" "$root/src/core/acts/tools/mw19/mw19_image.cpp" "$root/build/bin/Release/zlib.lib" `
        /Fe:mw19-schema-test.exe
    if ($LASTEXITCODE -ne 0) { throw 'MW2019 schema test compile failed' }
    & ./mw19-schema-test.exe "$root/config/data/mw19/schema.json"
    if ($LASTEXITCODE -ne 0) { throw 'MW2019 schema tests failed' }
    & $compiler /nologo /MD /EHsc /std:c++20 /DMW19_SCHEMA_STANDALONE `
        "/I$root/src/core/acts" "/I$root/deps/json/include" `
        "$root/scripts/mw19/image_test.cpp" "$root/src/core/acts/tools/mw19/mw19_image.cpp" /Fe:mw19-image-test.exe
    if ($LASTEXITCODE -ne 0) { throw 'MW2019 image test compile failed' }
    & ./mw19-image-test.exe
    if ($LASTEXITCODE -ne 0) { throw 'MW2019 image tests failed' }
    & $compiler /nologo /MD /EHsc /std:c++20 /DMW19_SCHEMA_STANDALONE `
        "/I$root/src/core/acts" "/I$root/deps/json/include" "/I$root/deps/zlib" `
        "$root/scripts/mw19/stream_buffer_test.cpp" "$root/src/core/acts/tools/mw19/mw19_schema.cpp" `
        "$root/src/core/acts/tools/mw19/mw19_payload.cpp" "$root/src/core/acts/tools/mw19/mw19_sound.cpp" "$root/src/core/acts/tools/mw19/mw19_image.cpp" "$root/build/bin/Release/zlib.lib" `
        /Fe:mw19-stream-buffer-test.exe
    if ($LASTEXITCODE -ne 0) { throw 'MW2019 stream buffer test compile failed' }
    & ./mw19-stream-buffer-test.exe
    if ($LASTEXITCODE -ne 0) { throw 'MW2019 stream buffer tests failed' }
    & $compiler /nologo /MD /EHsc /std:c++20 /DMW19_SCHEMA_STANDALONE `
        "/I$root/src/core/acts" "/I$root/deps/json/include" `
        "$root/scripts/mw19/mesh_test.cpp" "$root/src/core/acts/tools/mw19/mw19_mesh.cpp" /Fe:mw19-mesh-test.exe
    if ($LASTEXITCODE -ne 0) { throw 'MW2019 mesh test compile failed' }
    & ./mw19-mesh-test.exe
    if ($LASTEXITCODE -ne 0) { throw 'MW2019 mesh tests failed' }
    & $compiler /nologo /MD /EHsc /std:c++20 /DMW19_SCHEMA_STANDALONE `
        "/I$root/src/core/acts" "/I$root/deps/json/include" "/I$root/deps/zlib" `
        "$root/scripts/mw19/sound_test.cpp" "$root/src/core/acts/tools/mw19/mw19_sound.cpp" `
        "$root/src/core/acts/tools/mw19/mw19_schema.cpp" "$root/src/core/acts/tools/mw19/mw19_payload.cpp" `
        "$root/src/core/acts/tools/mw19/mw19_image.cpp" "$root/build/bin/Release/zlib.lib" /Fe:mw19-sound-test.exe
    if ($LASTEXITCODE -ne 0) { throw 'MW2019 sound test compile failed' }
    & ./mw19-sound-test.exe
    if ($LASTEXITCODE -ne 0) { throw 'MW2019 sound tests failed' }
} finally { Pop-Location }
