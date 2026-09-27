<#
.SYNOPSIS
  Run an Aether executable from a preset's build tree, with Vulkan validation layers
  enabled automatically when they have been provisioned (scripts/provision_validation.ps1).

.EXAMPLE
  ./scripts/run.ps1 -Exe sandbox
  ./scripts/run.ps1 -Preset wip-core-rhi -Exe sandbox --frames 300
  ./scripts/run.ps1 -Exe test.core

  Pass executable arguments directly after -Exe <name> (no `--` separator: Windows
  PowerShell 5.1 rejects a bare `--` with "parameter name '' is ambiguous").
#>
[CmdletBinding()]
param(
    [string]$Preset = "msvc-x64-debug",
    [Parameter(Mandatory = $true)][string]$Exe,
    [switch]$NoValidation,
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$ExeArgs
)

$ErrorActionPreference = "Stop"

$bin = "C:/Users/paulc/.aether/build/$Preset/bin"
$exePath = Join-Path $bin "$Exe.exe"
if (-not (Test-Path $exePath)) { throw "Not built: $exePath (run scripts/build.ps1 -Preset $Preset)" }

# Validation layers are built from source into this prefix (ADR-0002).
$vvlRoot = "C:/Users/paulc/.aether/tools/vvl"
if (-not $NoValidation -and (Test-Path $vvlRoot)) {
    $manifest = Get-ChildItem $vvlRoot -Recurse -Filter "VkLayer_khronos_validation.json" -ErrorAction SilentlyContinue |
                Select-Object -First 1
    if ($manifest) {
        $env:VK_LAYER_PATH = $manifest.DirectoryName
        # Sync validation + best-practices are opt-in; keep core validation always on.
        $env:AE_VALIDATION = "1"
        Write-Host "[run] Vulkan validation layer: $($manifest.DirectoryName)" -ForegroundColor Cyan
    }
} else {
    Write-Host "[run] Vulkan validation layer not provisioned (running without)." -ForegroundColor Yellow
}

Push-Location $bin
try {
    & $exePath @ExeArgs
    $code = $LASTEXITCODE
} finally {
    Pop-Location
}
exit $code
