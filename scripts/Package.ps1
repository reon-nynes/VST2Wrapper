param([Parameter(Mandatory)][string]$Version)
$ErrorActionPreference = 'Stop'
$workspace = Split-Path -Parent $PSScriptRoot
$versionName = [regex]::Replace($Version, '[^A-Za-z0-9._-]', '-')
if ([string]::IsNullOrWhiteSpace($versionName)) { throw 'Version must contain a release identifier' }
$destination = Join-Path $workspace 'out/release'
New-Item -ItemType Directory -Force $destination | Out-Null
$binary = Join-Path $destination "NyteraVST2Wrapper-$versionName-windows-x64.zip"
$source = Join-Path $destination "NyteraVST2Wrapper-$versionName-source.zip"
$prefix = "NyteraVST2Wrapper-$versionName-source/"
Push-Location $workspace
try {
    foreach ($asset in @('NyteraVST2Wrapper.exe', 'runtime/x86/NyteraBridge.exe', 'runtime/x64/NyteraBridge.exe', 'runtime/Effect.vst3', 'runtime/Instrument.vst3', 'LICENSE', 'LICENSES/AGPL-3.0-only.txt')) {
        if (-not (Test-Path -LiteralPath "out/package/$asset" -PathType Leaf)) { throw "Missing staged asset $asset" }
    }
    foreach ($dependency in @('JUCE', 'vst2sdk')) {
        if (-not (Test-Path -LiteralPath "out/deps/$dependency/.git/HEAD" -PathType Leaf)) { throw "Missing dependency checkout $dependency" }
    }
    Compress-Archive -Path 'out/package/*' -DestinationPath $binary -CompressionLevel Optimal -Force
    & git archive --format=zip "--prefix=$prefix" "--output=$source" HEAD
    if ($LASTEXITCODE -ne 0) { throw 'Source archive creation failed' }
    # The dependency checkouts contain the pinned revisions, applied JUCE changes
    # and Git metadata required by Dependencies.cmake for an offline rebuild.
    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [IO.Compression.ZipFile]::Open($source, [IO.Compression.ZipArchiveMode]::Update)
    try {
        foreach ($dependency in @('JUCE', 'vst2sdk')) {
            $root = Join-Path $workspace "out/deps/$dependency"
            foreach ($file in Get-ChildItem -LiteralPath $root -File -Recurse -Force) {
                $relative = [IO.Path]::GetRelativePath($workspace, $file.FullName).Replace('\', '/')
                [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $file.FullName, $prefix + $relative, [IO.Compression.CompressionLevel]::Optimal)
            }
        }
    }
    finally { $archive.Dispose() }
    $checksums = foreach ($file in @($binary, $source)) {
        $hash = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant()
        "$hash  $([IO.Path]::GetFileName($file))"
    }
    $checksums | Set-Content -LiteralPath (Join-Path $destination "NyteraVST2Wrapper-$versionName-SHA256SUMS.txt") -Encoding utf8NoBOM
    Write-Host "Packaged $binary"
    Write-Host "Packaged $source"
}
finally { Pop-Location }
