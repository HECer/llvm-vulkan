# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
<#
Native Windows Vulkan correctness and timing runner. Requires CMake, a C++17
compiler, Python3 and the Vulkan SDK on PATH. Builds the core only; the LLVM
plugin separately needs a matching LLVM build with usable plugin exports.
#>
[CmdletBinding()]
param(
    [string]$BuildDirectory = "build-rtx5090",
    [string]$OutputDirectory = "results/rtx5090",
    [string]$DeviceName = "5090",
    [string]$Generator = "Ninja",
    [switch]$SkipBuild,
    [switch]$Large
)
$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot
Push-Location $RepoRoot
$OldValidation = $env:LV_VALIDATION
$OldLayerEnables = $env:VK_LAYER_ENABLES
$OldInstanceLayers = $env:VK_INSTANCE_LAYERS
try {
    New-Item -ItemType Directory -Force $OutputDirectory | Out-Null
    if (-not $SkipBuild) {
        & cmake -S . -B $BuildDirectory -G $Generator -DCMAKE_BUILD_TYPE=Release -DLV_BUILD_LLVM_PLUGIN=OFF
        if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }
        & cmake --build $BuildDirectory --config Release --parallel
        if ($LASTEXITCODE -ne 0) { throw "CMake build failed" }
    }
    $Bench = Join-Path $BuildDirectory "lv-bench.exe"
    $Tests = Join-Path $BuildDirectory "lv-core-tests.exe"
    if (-not (Test-Path $Bench)) {
        $Bench = Join-Path $BuildDirectory "Release/lv-bench.exe"
        $Tests = Join-Path $BuildDirectory "Release/lv-core-tests.exe"
    }
    if (-not (Test-Path $Bench) -or -not (Test-Path $Tests)) {
        throw "Missing built executables"
    }
    $DevicesText = & $Bench --list-devices
    if ($LASTEXITCODE -ne 0) { throw "Vulkan device enumeration failed" }
    $DevicesText | Set-Content -Encoding utf8 (Join-Path $OutputDirectory "devices.json")
    $Devices = $DevicesText | ConvertFrom-Json
    $MatchingDevices = @($Devices | Where-Object { -not $_.is_cpu -and $_.vendor_id -eq 4318 -and $_.name -like "*$DeviceName*" })
    if ($MatchingDevices.Count -eq 0) {
        throw "No NVIDIA Vulkan device matching '$DeviceName'. A software result is not an RTX 5090 test."
    }
    if (@($MatchingDevices | Where-Object { -not $_.supports_int64 }).Count -gt 0) {
        throw "The selected device must expose shaderInt64 for the complete RTX test suite."
    }
    $env:LV_VALIDATION = "1"
    $env:VK_LAYER_ENABLES = "VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT"
    $LargeValue = if ($Large) { "1" } else { "0" }
    foreach ($Seed in @(5090, 1, 12345)) {
        & $Tests --backend vulkan --device gpu --device-name $DeviceName --cases 64 --seed $Seed --large $LargeValue 2>&1 |
            Tee-Object -FilePath (Join-Path $OutputDirectory "correctness-$Seed.log")
        if ($LASTEXITCODE -ne 0) { throw "Vulkan correctness/validation failed for seed $Seed" }
    }
    $env:LV_VALIDATION = "0"
    Remove-Item Env:VK_LAYER_ENABLES -ErrorAction SilentlyContinue
    Remove-Item Env:VK_INSTANCE_LAYERS -ErrorAction SilentlyContinue
    foreach ($Size in @(1024, 16384, 262144, 1048576)) {
        & $Bench --backend cpu --size $Size --repeat 7 --json (Join-Path $OutputDirectory "cpu-$Size.json")
        if ($LASTEXITCODE -ne 0) { throw "CPU benchmark failed" }
        & $Bench --backend vulkan --device gpu --device-name $DeviceName --size $Size --repeat 7 --json (Join-Path $OutputDirectory "vulkan-$Size.json")
        if ($LASTEXITCODE -ne 0) { throw "GPU benchmark failed" }
    }
    Write-Host "Completed. Reports are in $OutputDirectory. These are core kernel timings, not whole-compiler speedups."
} finally {
    $env:LV_VALIDATION = $OldValidation
    $env:VK_LAYER_ENABLES = $OldLayerEnables
    $env:VK_INSTANCE_LAYERS = $OldInstanceLayers
    Pop-Location
}
