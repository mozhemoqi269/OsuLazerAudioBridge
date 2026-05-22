param(
    [string]$Configuration = "Release",
    [string]$BuildDir = "build"
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    throw "vswhere.exe was not found. Install Visual Studio or Visual Studio Build Tools."
}

$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) {
    throw "No Visual Studio installation with C++ x64 tools was found."
}

$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) {
    throw "vcvars64.bat was not found at: $vcvars"
}

$buildPath = Join-Path $repoRoot $BuildDir
$configure = "cmake -S `"$repoRoot`" -B `"$buildPath`" -G `"NMake Makefiles`" -DCMAKE_BUILD_TYPE=$Configuration"
$build = "cmake --build `"$buildPath`""

cmd /c "call `"$vcvars`" && $configure && $build"
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}
