$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
Push-Location $projectRoot
try {
    cmake -S . -B build -A x64
    if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed' }
    cmake --build build --config Release
    if ($LASTEXITCODE -ne 0) { throw 'Release build failed' }
    ctest --test-dir build -C Release --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'Tests failed; artifact not copied' }
    New-Item -ItemType Directory -Path (Join-Path $projectRoot 'dist') -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $projectRoot 'build/Release/CS2FullReset.exe') -Destination (Join-Path $projectRoot 'dist/CS2FullReset.exe')
    Get-FileHash -LiteralPath (Join-Path $projectRoot 'dist/CS2FullReset.exe') -Algorithm SHA256
} finally {
    Pop-Location
}
