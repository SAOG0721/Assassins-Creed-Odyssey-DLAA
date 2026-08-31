[CmdletBinding()]
param(
    [string]$NgxSdkRoot,
    [string]$NgxRuntime,
    [string]$DlssNrRuntime,
    [string]$MinHookRoot = (Join-Path $PSScriptRoot 'third_party\minhook'),
    [string]$CMakePath,
    [string]$FxcPath
)

$ErrorActionPreference = 'Stop'
$buildDir = Join-Path $PSScriptRoot 'build'

if (-not $CMakePath) {
    $cmakeCommand = Get-Command cmake.exe -ErrorAction SilentlyContinue
    if ($cmakeCommand) { $CMakePath = $cmakeCommand.Source }
}
if (-not $CMakePath) {
    $bundledCMake = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    if (Test-Path -LiteralPath $bundledCMake) { $CMakePath = $bundledCMake }
}
if (-not $CMakePath -or -not (Test-Path -LiteralPath $CMakePath)) {
    throw 'CMake was not found. Install CMake or pass -CMakePath.'
}

$configure = @('-S', $PSScriptRoot, '-B', $buildDir, '-G', 'Visual Studio 17 2022', '-A', 'x64',
    "-DMINHOOK_ROOT=$MinHookRoot")
if ($NgxSdkRoot) { $configure += "-DNGX_SDK_ROOT=$NgxSdkRoot" }
if ($NgxRuntime) { $configure += "-DNGX_RUNTIME=$NgxRuntime" }
if ($DlssNrRuntime) { $configure += "-DDLSSNR_RUNTIME=$DlssNrRuntime" }
if ($FxcPath) { $configure += "-DFXC_EXECUTABLE=$FxcPath" }

& $CMakePath @configure
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed: $LASTEXITCODE" }

& $CMakePath --build $buildDir --config Release --parallel
if ($LASTEXITCODE -ne 0) { throw "Build failed: $LASTEXITCODE" }

$outputs = @(
    (Join-Path $buildDir 'bin\dinput8.dll'),
    (Join-Path $buildDir 'bin\ACOdysseyDLSSBridge.dll'),
    (Join-Path $buildDir 'bin\ACOdysseyDLAA.motion_decode.cso'),
    (Join-Path $buildDir 'bin\ACOdysseyDLAA.sharpen.cso'),
    (Join-Path $buildDir 'bin\ACOdysseyDLSSNR.encode.cso'),
    (Join-Path $buildDir 'bin\ACOdysseyDLSSNR.decode.cso'),
    (Join-Path $buildDir 'bin\ACOdysseyDLSSNR.guide_unpack.cso'),
    (Join-Path $buildDir 'bin\nvngx_dlss.dll'),
    (Join-Path $buildDir 'bin\nvngx_dlssnr.dll')
)
foreach ($output in $outputs) {
    if (-not (Test-Path -LiteralPath $output)) { throw "Build output missing: $output" }
}
Get-FileHash -Algorithm SHA256 -LiteralPath $outputs
