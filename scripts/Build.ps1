param([ValidateSet('Debug', 'Release')][string]$Configuration = 'Release')
$ErrorActionPreference = 'Stop'
$workspace = Split-Path -Parent $PSScriptRoot
Push-Location $workspace
try {
    $suffix = $Configuration.ToLowerInvariant()
    foreach ($architecture in @('x86', 'x64')) {
        & cmake --preset "$architecture-$suffix"
        if ($LASTEXITCODE -ne 0) { throw "CMake configuration failed for $architecture" }
        & cmake --build --preset "$architecture-$suffix"
        if ($LASTEXITCODE -ne 0) { throw "Build failed for $architecture" }
    }
    & cmake --build --preset "x64-$suffix" --target NyteraStage
    if ($LASTEXITCODE -ne 0) { throw 'Application staging failed' }
    Write-Host "Staged $Configuration application at $workspace\out\package\NyteraVST2Wrapper.exe"
}
finally { Pop-Location }
