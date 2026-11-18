# Builds bin\RE7DLSS-DX11.addon64 with MSVC (Visual Studio 2022+ with C++ tools).
# Usage: .\build.ps1 [-Install] [-Package] [-GameDir <folder of re7.exe>]
#   -Install  copies the add-on, nvngx_dlss.dll and the REFramework script into the game folder
#             (game closed). The game must be on the Steam beta branch dx11_non-rt.
#   -Package  makes bin\release\RE7DLSS-DX11-<version>.zip for Fluffy Mod Manager: one folder,
#             RE7DLSS-DX11, with modinfo.ini and the files as they go in the game folder, including
#             REFramework's TDB49 build (third_party\reframework\SOURCE.md).
param(
    [switch]$Install,
    [switch]$Package,
    [string]$GameDir = "E:\SteamLibrary\steamapps\common\RESIDENT EVIL 7 biohazard"
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$exe = "re7.exe"
$name = "RE7DLSS-DX11"

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
# vcvars64.bat runs vswhere by name; PATH entries with parentheses in odd places (a Steam tool's
# folder) break cmd's parsing of vcvars, so only the Windows folders are kept for the build.
$env:PATH = "$(Split-Path $vswhere);$env:SystemRoot\System32;$env:SystemRoot;$env:SystemRoot\System32\WindowsPowerShell\v1.0"
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "Visual Studio with the C++ x64 tools not found." }
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"

$obj = "$root\bin\obj"
New-Item -ItemType Directory -Force $obj | Out-Null
$addon = "$root\bin\$name.addon64"

# ReShade add-on headers in third_party\reshade match ReShade 6.8.0.
$cmd = @(
    "call `"$vcvars`" >nul",
    "fxc /nologo /T vs_5_0 /E VsMain /O3 /Vn kBlitVs /Fh `"$obj\blit_vs.h`" `"$root\src\blit.hlsl`" >nul",
    "fxc /nologo /T ps_5_0 /E PsMain /O3 /Vn kBlitPs /Fh `"$obj\blit_ps.h`" `"$root\src\blit.hlsl`" >nul",
    "fxc /nologo /T cs_5_0 /E main /O3 /Vn kRcasCs /Fh `"$obj\rcas_cs.h`" `"$root\src\rcas.hlsl`" >nul",
    "fxc /nologo /T vs_5_0 /E VsMain /O3 /Vn kDownVs /Fh `"$obj\down_vs.h`" `"$root\src\downsample.hlsl`" >nul",
    "fxc /nologo /T ps_5_0 /E PsMain /O3 /Vn kDownPs /Fh `"$obj\down_ps.h`" `"$root\src\downsample.hlsl`" >nul",
    "fxc /nologo /T cs_5_0 /E main /O3 /Vn kMvFixCs /Fh `"$obj\mvfix_cs.h`" `"$root\src\mvfix.hlsl`" >nul",
    "fxc /nologo /T cs_5_0 /E main /O3 /Vn kEffMaskCs /Fh `"$obj\effmask_cs.h`" `"$root\src\effmask.hlsl`" >nul",
    "fxc /nologo /T cs_5_0 /E main /O3 /Vn kEffViewCs /Fh `"$obj\effview_cs.h`" `"$root\src\effview.hlsl`" >nul",
    "fxc /nologo /T cs_5_0 /E main /O3 /Vn kExposureCs /Fh `"$obj\exposure_cs.h`" `"$root\src\exposure.hlsl`" >nul",
    ("cl /nologo /std:c++17 /O2 /EHsc /MT /W3 /D_CRT_SECURE_NO_WARNINGS /DNOMINMAX " +
     "/I `"$obj`" /I `"$root\third_party\imgui`" /I `"$root\third_party\reshade\include`" /I `"$root\third_party\dlss\include`" " +
     "/I `"$root\third_party\fidelityfx\api\include`" /I `"$root\third_party\fidelityfx\upscalers\include`" " +
     "/Fo`"$obj\\`" `"$root\src\main.cpp`" `"$root\src\upscaler.cpp`" `"$root\src\dlss.cpp`" " +
     "`"$root\src\bridge.cpp`" `"$root\src\fsr.cpp`" " +
     "user32.lib advapi32.lib ole32.lib d3d11.lib d3d12.lib dxgi.lib `"$root\third_party\dlss\lib\nvsdk_ngx_s.lib`" " +
     "/link /DLL /OUT:`"$addon`" /PDB:`"$obj\$name.pdb`" /DEBUG")
) -join " && "
cmd /c $cmd
if ($LASTEXITCODE) { throw "build failed" }
Write-Host "Built $addon"

# NVIDIA's signed DLSS runtime (310.9.16; not in git).
$ngx = "$root\third_party\bin\nvngx_dlss.dll"
# AMD's signed FSR runtime (FSR SDK 2.4.0, Kits\FidelityFX\signedbin; not in git), next to the exe.
$ffx = "$root\third_party\fidelityfx\bin"
$ffxFiles = "amd_fidelityfx_loader_dx12.dll", "amd_fidelityfx_upscaler_dx12.dll"
foreach ($f in $ffxFiles) { if (($Install -or $Package) -and -not (Test-Path "$ffx\$f")) { throw "$f not found in $ffx" } }
if ($Install) {
    if (-not (Test-Path $ngx)) { throw "nvngx_dlss.dll not found in third_party\bin" }
    if (-not (Test-Path "$GameDir\$exe")) { throw "$exe not found in $GameDir (pass -GameDir)" }
    if (Get-Process ([IO.Path]::GetFileNameWithoutExtension($exe)) -ErrorAction SilentlyContinue) { throw "Close the game first." }
    Copy-Item $addon, $ngx $GameDir -Force
    foreach ($f in $ffxFiles) { Copy-Item "$ffx\$f" $GameDir -Force }
    # REFramework companion script: sets the render scale for the render resolution dropdown.
    if (Test-Path "$GameDir\reframework") {
        New-Item -ItemType Directory -Force "$GameDir\reframework\autorun" | Out-Null
        Copy-Item "$root\reframework\$name.lua" "$GameDir\reframework\autorun" -Force
    } else { Write-Warning "REFramework not installed: the render resolution dropdown needs it" }
    Write-Host "Installed to $GameDir"
}

if ($Package) {
    $refw = "$root\third_party\reframework\dinput8.dll"
    if (-not (Test-Path $ngx)) { throw "nvngx_dlss.dll not found in third_party\bin" }
    if (-not (Test-Path $refw)) { throw "dinput8.dll not found in third_party\reframework (see SOURCE.md there)" }
    $version = [regex]::Match((Get-Content "$root\src\version.h" -Raw), 'RD_VERSION "([^"]+)"').Groups[1].Value
    $stage = "$root\bin\release\$name-$version"
    $mod = "$stage\$name"
    if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
    New-Item -ItemType Directory -Force "$mod\reframework\autorun", "$mod\$name-licenses" | Out-Null

    # Files installed into the game folder (where re7.exe is).
    Copy-Item $addon, $ngx, $refw $mod
    foreach ($f in $ffxFiles) { Copy-Item "$ffx\$f" $mod }
    Copy-Item "$root\reframework\$name.lua" "$mod\reframework\autorun"
    Copy-Item "$root\README.md" "$mod\$name-README.md"
    Copy-Item "$root\THIRD_PARTY_NOTICES.txt" "$mod\$name-THIRD_PARTY_NOTICES.txt"
    Copy-Item "$root\LICENSE" "$mod\$name-licenses\$name-LICENSE.txt"
    Copy-Item "$root\third_party\dlss\LICENSE.txt" "$mod\$name-licenses\NVIDIA-DLSS-LICENSE.txt"
    Copy-Item "$root\third_party\reshade\LICENSE.md" "$mod\$name-licenses\ReShade-LICENSE.md"
    Copy-Item "$root\third_party\reframework\LICENSE.txt" "$mod\$name-licenses\REFramework-LICENSE.txt"
    Copy-Item "$root\third_party\fidelityfx\license.md" "$mod\$name-licenses\AMD-FSR-LICENSE.txt"

    # Fluffy Mod Manager's mod information (key=value, one per line).
    @(
        "name=$name - DLSS and FSR for Resident Evil 7 (DirectX 11 version)",
        "version=v$version",
        "description=For the DirectX 11 (non-ray-tracing) version of Resident Evil 7 (Steam beta branch dx11_non-rt). Replaces the game's TAA with NVIDIA DLSS or AMD FSR 3.1 / FSR 4: native anti-aliasing, or upscaling with the game's bloom, tonemapping, lens effects and film grain at full resolution; an effect mask against ghosting on particles and effects; sharper textures and a sharpening slider. Includes REFramework (TDB49 build) for the render resolution dropdown. DLSS needs an NVIDIA RTX card, FSR any Direct3D 12 capable card; ReShade 6.8+ with add-on support. Settings: ReShade overlay (Home), Add-ons tab, $name; F9 switches upscaling on and off.",
        "author=GreatDanish"
    ) | Set-Content "$mod\modinfo.ini" -Encoding ascii

    # Zip with forward-slash entry names (Compress-Archive in PowerShell 5.1 writes backslashes).
    Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
    $zip = "$root\bin\release\$name-$version.zip"
    if (Test-Path $zip) { Remove-Item $zip -Force }
    $archive = [IO.Compression.ZipFile]::Open($zip, [IO.Compression.ZipArchiveMode]::Create)
    try {
        Get-ChildItem $stage -Recurse -File | ForEach-Object {
            $entry = $_.FullName.Substring($stage.Length + 1).Replace('\', '/')
            [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $_.FullName, $entry,
                [IO.Compression.CompressionLevel]::Optimal)
        }
    } finally { $archive.Dispose() }
    Write-Host "Packaged $zip"
    Get-ChildItem $stage -Recurse -File | ForEach-Object {
        "{0,-60} {1,12:N0}" -f $_.FullName.Substring($stage.Length + 1), $_.Length
    }
}
