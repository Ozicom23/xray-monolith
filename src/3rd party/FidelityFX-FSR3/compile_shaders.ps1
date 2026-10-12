# Compiles the FSR 3.1 upscaler passes for D3D11 (cs_5_0) with fxc into C headers.
# Usage: powershell -File compile_shaders.ps1 [-OutDir <dir>]
# Output: <OutDir>/<pass>_s<0|1>.h with byte arrays g_<pass>_s<0|1> (s = sharpening permutation).

param(
    [string]$OutDir = "$PSScriptRoot\generated"
)

$ErrorActionPreference = "Stop"

$fxc = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\*\x64\fxc.exe" | Sort-Object FullName -Descending | Select-Object -First 1
if (-not $fxc) { throw "fxc.exe not found in the Windows SDK" }
Write-Host "Using $($fxc.FullName)"

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$gpu = "$PSScriptRoot\include\FidelityFX\gpu"

# Fixed configuration used by the engine: LDR color, render-resolution motion vectors without jitter,
# regular (non-inverted) depth. Only sharpening is a runtime permutation.
$defines = @(
    "FFX_GPU=1", "FFX_HLSL=1", "FFX_HLSL_SM=50", "FFX_HALF=0", "FFX_PREFER_WAVE64=",
    "FFX_SPD_NO_WAVE_OPERATIONS=1",
    "FFX_FSR3UPSCALER_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF=0",
    "FFX_FSR3UPSCALER_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF=0",
    "FFX_FSR3UPSCALER_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF=0",
    "FFX_FSR3UPSCALER_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF=0",
    "FFX_FSR3UPSCALER_OPTION_UPSAMPLE_USE_LANCZOS_TYPE=2",
    "FFX_FSR3UPSCALER_OPTION_REPROJECT_USE_LANCZOS_TYPE=0",
    "FFX_FSR3UPSCALER_OPTION_HDR_COLOR_INPUT=0",
    "FFX_FSR3UPSCALER_OPTION_LOW_RESOLUTION_MOTION_VECTORS=1",
    "FFX_FSR3UPSCALER_OPTION_JITTERED_MOTION_VECTORS=0",
    "FFX_FSR3UPSCALER_OPTION_INVERTED_DEPTH=0"
)

$failed = 0
foreach ($shader in Get-ChildItem "$PSScriptRoot\shaders\*.hlsl") {
    $name = $shader.BaseName
    foreach ($sharpen in 0, 1) {
        $var = "g_${name}_s${sharpen}"
        $out = Join-Path $OutDir "${name}_s${sharpen}.h"
        $fxcArgs = @("/nologo", "/T", "cs_5_0", "/E", "CS", "/O3", "/Vn", $var, "/Fh", $out,
                  "/I", $gpu, "/I", "$gpu\fsr3upscaler")
        foreach ($d in $defines + "FFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING=$sharpen") { $fxcArgs += @("/D", $d) }
        $fxcArgs += $shader.FullName

        & $fxc.FullName @args
        if ($LASTEXITCODE -ne 0) {
            Write-Host "FAILED: $name (sharpen=$sharpen)"
            $failed++
        }
    }
}

if ($failed -gt 0) { throw "$failed shader permutation(s) failed to compile" }
Write-Host "All FSR shaders compiled to $OutDir"
