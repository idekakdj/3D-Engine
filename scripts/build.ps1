<#
.SYNOPSIS
  Aether Engine build wrapper (ADR-0001). Imports the MSVC environment from
  vcvars64.bat, puts the VS-bundled CMake + Ninja on PATH, then configures/builds
  via CMake presets. Every agent and CI path builds through THIS script so that
  cl.exe / cmake / ninja are always resolvable regardless of the caller's shell.

.EXAMPLE
  ./scripts/build.ps1                       # configure (if needed) + build Debug
  ./scripts/build.ps1 -Preset msvc-x64-release
  ./scripts/build.ps1 -Target aether.core   # build a single target
  ./scripts/build.ps1 -Reconfigure          # force a fresh configure
  ./scripts/build.ps1 -Test                 # build then run ctest
  ./scripts/build.ps1 -Preset wip-scene -Target test.scene -Jobs 2   # low-memory retry
#>
[CmdletBinding()]
param(
    [string]$Preset = "msvc-x64-debug",
    [string]$Target = "",
    [switch]$Reconfigure,
    [switch]$Clean,
    [switch]$Test,
    [int]$Jobs = 0      # override the preset's job count (e.g. -Jobs 2 after an MSVC out-of-heap error)
)

$ErrorActionPreference = "Stop"

# --- Locate Visual Studio + bundled tools -----------------------------------
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found; is Visual Studio (Build Tools) installed?" }
$vsPath = (& $vswhere -latest -products * -property installationPath).Trim()
if (-not $vsPath) { throw "Could not resolve a Visual Studio installation path." }

$vcvars   = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
$cmakeExe = Join-Path $vsPath "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$ninjaDir = Join-Path $vsPath "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"
$ctestExe = Join-Path $vsPath "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe"

foreach ($p in @($vcvars, $cmakeExe, $ninjaDir)) {
    if (-not (Test-Path $p)) { throw "Required tool not found: $p" }
}

# --- Import the MSVC dev environment (once per process) ----------------------
if (-not $env:VSCMD_ARG_TGT_ARCH) {
    Write-Host "[build] Importing MSVC x64 environment from vcvars64.bat ..." -ForegroundColor Cyan
    cmd /c "`"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
        if ($_ -match '^(.*?)=(.*)$') {
            Set-Item -Path "env:$($matches[1])" -Value $matches[2]
        }
    }
}
# Ensure the bundled Ninja is resolvable by CMake.
$env:PATH = "$ninjaDir;$env:PATH"

$repoRoot = Split-Path $PSScriptRoot -Parent
$buildDir = "C:/Users/paulc/.aether/build/$Preset"

if ($Clean -and (Test-Path $buildDir)) {
    Write-Host "[build] Removing $buildDir" -ForegroundColor Yellow
    Remove-Item -Recurse -Force $buildDir
}

# --- Configure ---------------------------------------------------------------
$cacheFile = Join-Path $buildDir "CMakeCache.txt"
if ($Reconfigure -or -not (Test-Path $cacheFile)) {
    Write-Host "[build] Configuring preset '$Preset' ..." -ForegroundColor Cyan
    Push-Location $repoRoot
    & $cmakeExe --preset $Preset
    $code = $LASTEXITCODE
    Pop-Location
    if ($code -ne 0) { throw "CMake configure failed ($code)." }
}

# --- Build -------------------------------------------------------------------
$buildArgs = @("--build", "--preset", $Preset)
if ($Target) { $buildArgs += @("--target", $Target) }
if ($Jobs -gt 0) { $buildArgs += @("-j", "$Jobs") }
Write-Host "[build] Building $(if($Target){$Target}else{'ALL'}) ($Preset) ..." -ForegroundColor Cyan
& $cmakeExe @buildArgs
$code = $LASTEXITCODE
if ($code -ne 0) { throw "Build failed ($code)." }

# --- Test (optional) ---------------------------------------------------------
if ($Test) {
    Write-Host "[build] Running ctest ..." -ForegroundColor Cyan
    & $ctestExe --test-dir $buildDir --output-on-failure
    $code = $LASTEXITCODE
    if ($code -ne 0) { throw "Tests failed ($code)." }
}

Write-Host "[build] OK -> $buildDir/bin" -ForegroundColor Green
