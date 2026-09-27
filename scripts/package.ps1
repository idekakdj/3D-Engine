<#
.SYNOPSIS
  Builds the downloadable Aether packages (ADR-0013): a portable ZIP and, when Inno Setup 6 is
  installed, AetherSetup (Windows installer with Start menu / desktop shortcuts and uninstaller).

.DESCRIPTION
  1. Builds aether-editor and aether-player with the Release preset (scripts/build.ps1).
  2. Runs CPack on the "Aether" install component (cmake/Packaging.cmake).
  3. Copies the packages to <repo>/dist/.
  4. With -SmokeTest: unzips the ZIP to a temp folder and runs the INSTALLED editor self-test and
     player check against a temporary Documents folder, then verifies that nothing was written
     into the install folder.

.EXAMPLE
  ./scripts/package.ps1                  # build + package
  ./scripts/package.ps1 -SmokeTest       # ... and test the packaged build
  ./scripts/package.ps1 -SkipBuild       # re-package the existing Release build

  Inno Setup (free) for the setup .exe:  winget install JRSoftware.InnoSetup
#>
[CmdletBinding()]
param(
    [string]$Preset = "msvc-x64-release",
    [switch]$SkipBuild,
    [switch]$SmokeTest
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path $PSScriptRoot -Parent
$buildDir = "C:/Users/paulc/.aether/build/$Preset"
$distDir  = Join-Path $repoRoot "dist"

# --- tools (same Visual Studio bundle as build.ps1) --------------------------------------------
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found; is Visual Studio (Build Tools) installed?" }
$vsPath   = (& $vswhere -latest -products * -property installationPath).Trim()
$cmakeBin = Join-Path $vsPath "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
$cmakeExe = Join-Path $cmakeBin "cmake.exe"
$cpackExe = Join-Path $cmakeBin "cpack.exe"
foreach ($p in @($cmakeExe, $cpackExe)) { if (-not (Test-Path $p)) { throw "Required tool not found: $p" } }

# --- 1. build ------------------------------------------------------------------------------------
if (-not $SkipBuild) {
    foreach ($t in @("aether-editor", "aether-player")) {
        & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "build.ps1") -Preset $Preset -Target $t
        if ($LASTEXITCODE -ne 0) { throw "Build of $t failed ($LASTEXITCODE)." }
    }
}
if (-not (Test-Path (Join-Path $buildDir "CPackConfig.cmake"))) { throw "No CPack config in $buildDir (build first)." }

# --- 2. package ----------------------------------------------------------------------------------
# Inno Setup is looked up at configure time; point the cache at it if it was installed since.
$iscc = @(
    "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
    "${env:ProgramFiles}\Inno Setup 6\ISCC.exe",
    "${env:LOCALAPPDATA}\Programs\Inno Setup 6\ISCC.exe"
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if ($iscc) {
    Write-Host "[package] Inno Setup: $iscc" -ForegroundColor Cyan
    & $cmakeExe -S $repoRoot -B $buildDir "-DAE_ISCC_EXECUTABLE=$iscc" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "CMake reconfigure failed ($LASTEXITCODE)." }
} else {
    Write-Host "[package] Inno Setup 6 not found: building the ZIP only." -ForegroundColor Yellow
    Write-Host "          For AetherSetup.exe run:  winget install JRSoftware.InnoSetup" -ForegroundColor Yellow
}

$pkgDir = Join-Path $buildDir "packages"
if (Test-Path $pkgDir) { Remove-Item -Recurse -Force $pkgDir }
Write-Host "[package] Running CPack ..." -ForegroundColor Cyan
Push-Location $buildDir
& $cpackExe --config CPackConfig.cmake
$code = $LASTEXITCODE
Pop-Location
if ($code -ne 0) { throw "CPack failed ($code)." }

New-Item -ItemType Directory -Force $distDir | Out-Null
$packages = Get-ChildItem $pkgDir -File | Where-Object { $_.Extension -in ".zip", ".exe" }
foreach ($p in $packages) {
    Copy-Item $p.FullName $distDir -Force
    Write-Host ("[package] {0}  ({1:N1} MB)" -f (Join-Path $distDir $p.Name), ($p.Length / 1MB)) -ForegroundColor Green
}

# --- 3. smoke test of the packaged build ------------------------------------------------------
if ($SmokeTest) {
    $zip = $packages | Where-Object { $_.Extension -eq ".zip" } | Select-Object -First 1
    if (-not $zip) { throw "No ZIP package to test." }
    $root = Join-Path $env:TEMP ("aether-smoke-" + [guid]::NewGuid().ToString("N").Substring(0, 8))
    $docs = Join-Path $root "Documents"
    Expand-Archive $zip.FullName -DestinationPath $root
    $app = Get-ChildItem $root -Directory | Where-Object { $_.Name -like "Aether-*" } | Select-Object -First 1
    $hash = { Get-ChildItem $app.FullName -Recurse -File | Get-FileHash | ForEach-Object { "$($_.Hash) $($_.Path)" } }
    $before = & $hash

    $env:AETHER_DOCUMENTS_DIR = $docs
    try {
        Write-Host "[smoke] editor self-test (installed layout) ..." -ForegroundColor Cyan
        & (Join-Path $app.FullName "bin\aether-editor.exe") --self-test
        $editor = $LASTEXITCODE
        Write-Host "[smoke] player check ..." -ForegroundColor Cyan
        & (Join-Path $app.FullName "bin\aether-player.exe") --frames 120 --check
        $player = $LASTEXITCODE
    } finally {
        Remove-Item Env:AETHER_DOCUMENTS_DIR
    }
    $after = & $hash
    $unchanged = -not (Compare-Object $before $after)
    $project = Test-Path (Join-Path $docs "Aether Projects\Starter Project\Starter Project.aeproject")

    Write-Host ""
    Write-Host ("[smoke] editor self-test : {0}" -f $(if ($editor -eq 0) { "PASSED" } else { "FAILED ($editor)" }))
    Write-Host ("[smoke] player check     : {0}" -f $(if ($player -eq 0) { "PASSED" } else { "FAILED ($player)" }))
    Write-Host ("[smoke] starter project  : {0}" -f $(if ($project) { "created in Documents" } else { "MISSING" }))
    Write-Host ("[smoke] install folder   : {0}" -f $(if ($unchanged) { "unchanged" } else { "MODIFIED (bug!)" }))
    if ($editor -ne 0 -or $player -ne 0 -or -not $project -or -not $unchanged) {
        throw "Smoke test failed (files kept in $root)."
    }
    Remove-Item -Recurse -Force $root
    Write-Host "[smoke] OK" -ForegroundColor Green
}
