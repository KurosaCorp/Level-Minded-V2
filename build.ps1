<#
.SYNOPSIS
    Configures and builds LevelMinded with MSVC + Ninja, no Developer Prompt needed.
.EXAMPLE
    .\build.ps1                  # Release build
    .\build.ps1 -Preset debug    # Debug build
    .\build.ps1 -Run             # Release build, then launch
    .\build.ps1 -Test            # Release build, then run unit tests
#>
param(
    [ValidateSet('release', 'debug', 'profile')]
    [string]$Preset = 'release',
    [switch]$Run,
    [switch]$Test,
    [switch]$Clean
)
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

# Load the MSVC x64 environment (compiler, Windows SDK, bundled CMake + Ninja) once per shell.
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) { throw 'vswhere.exe not found. Install Visual Studio 2022+ with "Desktop development with C++".' }
    $vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vsPath) { throw 'No Visual Studio install with the MSVC x64 toolset was found.' }

    $env:PATH = "$(Split-Path $vswhere);$env:PATH" # vcvars64.bat calls vswhere by name
    $vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
    cmd /s /c "`"$vcvars`" >nul && set" | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] }
    }
    # vcvars adds these only when the CMake component is installed; add them explicitly to be safe.
    $cmakeRoot = Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake'
    $env:PATH = "$cmakeRoot\CMake\bin;$cmakeRoot\Ninja;$env:PATH"
}

$buildDir = Join-Path $PSScriptRoot "build\$Preset"
if ($Clean -and (Test-Path $buildDir)) { Remove-Item -Recurse -Force $buildDir }

cmake --preset $Preset
if ($LASTEXITCODE) { exit $LASTEXITCODE }
cmake --build --preset $Preset
if ($LASTEXITCODE) { exit $LASTEXITCODE }

$exe = Join-Path $buildDir 'bin\LevelMinded.exe'
Write-Host "Built $exe"
if ($Test) {
    ctest --test-dir $buildDir --output-on-failure
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
}
if ($Run) { Start-Process $exe -WorkingDirectory (Split-Path $exe) }
