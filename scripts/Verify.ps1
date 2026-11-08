param(
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [string]$ReaperPath,
    [ValidateSet('Audio', 'State', 'Automation', 'Editors', 'Recovery', 'Relocation')][string]$AcceptanceScript = 'Audio'
)
$ErrorActionPreference = 'Stop'
$workspace = Split-Path -Parent $PSScriptRoot
Push-Location $workspace
try {
    foreach ($architecture in @('x86', 'x64')) {
        & ctest --test-dir "out/build/$architecture" -C $Configuration --output-on-failure
        if ($LASTEXITCODE -ne 0) { throw "Tests failed for $architecture $Configuration" }
    }
    $package = Join-Path $workspace 'out/package'
    foreach ($asset in @('NyteraVST2Wrapper.exe', 'runtime/x86/NyteraBridge.exe', 'runtime/x64/NyteraBridge.exe', 'runtime/Effect.vst3', 'runtime/Instrument.vst3')) {
        if (-not (Test-Path -LiteralPath (Join-Path $package $asset) -PathType Leaf)) { throw "Missing staged asset $asset" }
    }
    if (-not $ReaperPath) { return }
    if (-not [IO.Path]::IsPathFullyQualified($ReaperPath) -or -not (Test-Path -LiteralPath $ReaperPath -PathType Leaf)) {
        throw 'ReaperPath must name an existing absolute executable path'
    }
    $profile = Join-Path $workspace 'out/reaper-acceptance'
    $plugins = Join-Path $profile 'TestPlugins'
    New-Item -ItemType Directory -Force $plugins, "$profile/renders", "$profile/isolated-common/VST3", "$profile/isolated-local" | Out-Null
    $probe = Join-Path $workspace "out/build/x64/$Configuration/NyteraBridgeProbe.exe"
    foreach ($fixture in @(@('Delay', 'x86', 'Delay'), @('Instrument', 'x64', 'Midi'), @('State', 'x86', 'Chunk'), @('Program', 'x64', 'Program'))) {
        $variant, $architecture, $label = $fixture
        $helper = Join-Path $package "runtime/$architecture/NyteraBridge.exe"
        $source = Join-Path $workspace "out/build/$architecture/$Configuration/Nytera${variant}Fixture.dll"
        $manifest = Join-Path $profile "Fixture$variant.manifest"
        & $helper --inspect $source --output $manifest
        if ($LASTEXITCODE -ne 0) { throw "Fixture inspection failed for $variant" }
        $template = if ($variant -eq 'Instrument') { "$package/runtime/Instrument.vst3" } else { "$package/runtime/Effect.vst3" }
        & $probe --package $template $helper $source $manifest "$plugins/Fixture$label.vst3"
        if ($LASTEXITCODE -ne 0) { throw "Fixture packaging failed for $variant" }
    }
    if ($AcceptanceScript -eq 'Relocation') {
        $portableSource = Join-Path $profile 'Portable Source.dll'
        Copy-Item -LiteralPath "$workspace/out/build/x64/$Configuration/NyteraFixture.dll" -Destination $portableSource -Force
        $manifest = Join-Path $profile 'Portable.manifest'
        & "$package/runtime/x64/NyteraBridge.exe" --inspect $portableSource --output $manifest
        if ($LASTEXITCODE -ne 0) { throw 'Portable fixture inspection failed' }
        $original = Join-Path $profile 'Portable Original.vst3'
        & $probe --package "$package/runtime/Effect.vst3" "$package/runtime/x64/NyteraBridge.exe" $portableSource $manifest $original
        if ($LASTEXITCODE -ne 0) { throw 'Portable fixture packaging failed' }
        Remove-Item -LiteralPath $portableSource
        Move-Item -LiteralPath $original -Destination "$plugins/Renamed Portable.vst3" -Force
    }
    if ($AcceptanceScript -eq 'Editors') {
        foreach ($name in @('Decapitator', 'LittleAlterBoy', 'BBE Sonic Maximizer', 'VPRE-73')) {
            $converted = Join-Path $workspace "out/gui-generated/$name.vst3"
            if (-not (Test-Path -LiteralPath $converted)) { throw "Generate $name through the staged application first" }
            Copy-Item -LiteralPath $converted -Destination $plugins -Force
        }
    }
    if (-not (Test-Path -LiteralPath "$profile/reaper.ini")) {
        @"
[REAPER]
vstpath64=$plugins
vst_scan=2
renderclosewhendone=4
[audioconfig]
mode=4
dummy_srate=44100
dummy_blocksize=1024
allow_sr_override=1
"@ | Set-Content -LiteralPath "$profile/reaper.ini"
    }
    $launch = [Diagnostics.ProcessStartInfo]::new($ReaperPath)
    $launch.UseShellExecute = $false
    $launch.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
    $launch.Environment['CommonProgramFiles'] = "$profile/isolated-common"
    $launch.Environment['CommonProgramW6432'] = "$profile/isolated-common"
    $launch.Environment['LOCALAPPDATA'] = "$profile/isolated-local"
    if (-not (Test-Path -LiteralPath "$profile/.nytera-profile-ready")) {
        # REAPER appends system VST3 paths during first-profile migration. Disable
        # scanning for that migration, then replace the path list before testing.
        $initial = Get-Content -LiteralPath "$profile/reaper.ini" -Raw
        if ($initial -match '(?m)^vst_scan=') { $initial = $initial -replace '(?m)^vst_scan=.*$', 'vst_scan=2' }
        else { $initial = $initial -replace '\[REAPER\]', "[REAPER]`nvst_scan=2" }
        Set-Content -LiteralPath "$profile/reaper.ini" -Value $initial
        'reaper.defer(function() reaper.Main_OnCommand(40004, 0) end)' | Set-Content -LiteralPath "$profile/initialize.lua"
        foreach ($argument in @('-newinst', '-nosplash', '-cfgfile', "$profile/reaper.ini", "$profile/initialize.lua")) { $launch.ArgumentList.Add($argument) }
        $initializing = [Diagnostics.Process]::Start($launch)
        if (-not $initializing.WaitForExit(15000)) {
            $initializing.Kill($true)
            $initializing.WaitForExit()
            throw 'Dedicated REAPER profile initialization did not finish'
        }
        'Initialized with startup scanning disabled' | Set-Content -LiteralPath "$profile/.nytera-profile-ready"
        $launch.ArgumentList.Clear()
    }
    $settings = Get-Content -LiteralPath "$profile/reaper.ini" -Raw
    $settings = [regex]::Replace($settings, '(?m)^vstpath64=.*$', "vstpath64=$plugins")
    $settings = [regex]::Replace($settings, '(?m)^vst_scan=.*$', 'vst_scan=0')
    Set-Content -LiteralPath "$profile/reaper.ini" -Value $settings
    "<REAPER_PROJECT 0.1 7.85 1`n>`n" | Set-Content -LiteralPath "$profile/empty-audio.rpp"
    Copy-Item -LiteralPath "tests/reaper/$AcceptanceScript.lua" -Destination "$profile/bootstrap.lua" -Force
    foreach ($argument in @('-newinst', '-nosplash', '-cfgfile', "$profile/reaper.ini", "$profile/bootstrap.lua")) { $launch.ArgumentList.Add($argument) }
    $process = [Diagnostics.Process]::Start($launch)
    $process.Id | Set-Content -LiteralPath "$profile/process-id.txt"
    Write-Host "Started dedicated REAPER acceptance process $($process.Id) using $profile"
    Write-Host 'Close completed-render dialogs to advance between projects. Automation.lua requests one click in the original fixture editor.'
}
finally { Pop-Location }
