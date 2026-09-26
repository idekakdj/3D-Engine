<#
.SYNOPSIS
  Provision VK_LAYER_KHRONOS_validation from source (ADR-0002). The machine has no
  Vulkan SDK, so the Khronos validation layer is cloned, built with the VS-bundled
  CMake + Ninja under the vcvars64 environment, and installed into
  C:/Users/paulc/.aether/tools/vvl, where scripts/run.ps1 discovers it.

.DESCRIPTION
  Idempotent: a successful install writes a stamp file; re-running with the same tag
  is a no-op that just prints the manifest directory. An interrupted run resumes:
  the clone is reused when it is at the right commit, dependencies that update_deps.py
  already installed are skipped, and Ninja rebuilds incrementally.

  Layout (all outside the repository):
    C:/Users/paulc/.aether/tools/vvl-src           shallow clone at -Tag
    C:/Users/paulc/.aether/tools/vvl-src/external  deps fetched/built by update_deps.py
    C:/Users/paulc/.aether/tools/vvl-src/build     VVL build tree
    C:/Users/paulc/.aether/tools/vvl               install prefix (bin/ holds the dll + json)
    C:/Users/paulc/.aether/tools/vvl-provision.log log of the last run

  Memory: the 16 GB machine is shared with other concurrent builds, so parallelism is
  capped at -Jobs for both the dependency builds (update_deps.py honours
  PYTHON_CPU_COUNT on Python >= 3.13) and the layer build. On an MSVC out-of-memory
  failure the failing step is retried with fewer jobs (6 -> 3 -> 2 -> 1).

  The manifest directory is written to the success stream (last line) so callers can
  capture it: $dir = ./scripts/provision_validation.ps1

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts/provision_validation.ps1
  powershell -ExecutionPolicy Bypass -File scripts/provision_validation.ps1 -Jobs 3
  powershell -ExecutionPolicy Bypass -File scripts/provision_validation.ps1 -Force      # clean rebuild
  powershell -ExecutionPolicy Bypass -File scripts/provision_validation.ps1 -Tag vulkan-sdk-1.4.328.1
#>
[CmdletBinding()]
param(
    [string]$Tag = "vulkan-sdk-1.4.328.0",
    [ValidateRange(1, 64)][int]$Jobs = 6,
    [switch]$Force
)

$ErrorActionPreference = "Stop"

$RepoUrl    = "https://github.com/KhronosGroup/Vulkan-ValidationLayers"
$ToolsRoot  = "C:/Users/paulc/.aether/tools"
$SrcDir     = "$ToolsRoot/vvl-src"
$BuildDir   = "$SrcDir/build"
$InstallDir = "$ToolsRoot/vvl"
$StampFile  = "$InstallDir/aether_provisioned.txt"
$LogFile    = "$ToolsRoot/vvl-provision.log"
$ManifestName = "VkLayer_khronos_validation.json"
$LayerDll     = "VkLayer_khronos_validation.dll"

# MSVC / linker diagnostics that indicate memory contention rather than a real error.
$OomPattern = '(?i)\b(C1060|C1076|C3859|C1002|C1001|D8040|LNK1102|LNK1248)\b|out of heap|heap space|out of memory|not enough memory|insufficient memory|not enough space|0xC0000017|bad_alloc'

New-Item -ItemType Directory -Force $ToolsRoot | Out-Null
$script:LogWriter = New-Object System.IO.StreamWriter($LogFile, $false, [System.Text.UTF8Encoding]::new($false))
$script:LogWriter.AutoFlush = $true

function Write-Log {
    param([string]$Message, [string]$Color = "")
    if ($Color) { Write-Host $Message -ForegroundColor $Color } else { Write-Host $Message }
    $script:LogWriter.WriteLine($Message)
}

# Runs a native command, streaming stdout+stderr to the host and the log. Sets
# $script:SawOom when a line matches $OomPattern. Returns the exit code.
function Invoke-Native {
    param([string]$Exe, [string[]]$Arguments)
    Write-Log "[vvl] > $Exe $($Arguments -join ' ')" "DarkGray"
    $script:SawOom = $false
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = "Continue"   # native stderr must not become a terminating error
    try {
        & $Exe @Arguments 2>&1 | ForEach-Object {
            $line = "$_"
            if ($line -match $OomPattern) { $script:SawOom = $true }
            Write-Log $line
        }
        $code = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $prevEap
    }
    return $code
}

# Runs a native command and returns its trimmed stdout lines (stderr discarded).
function Get-NativeOutput {
    param([string]$Exe, [string[]]$Arguments)
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $out = & $Exe @Arguments 2>$null
        $script:LastNativeCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $prevEap
    }
    return @($out | ForEach-Object { "$_".Trim() } | Where-Object { $_ })
}

function Find-Manifest {
    if (-not (Test-Path $InstallDir)) { return $null }
    return Get-ChildItem $InstallDir -Recurse -Filter $ManifestName -ErrorAction SilentlyContinue |
           Select-Object -First 1
}

function Get-NextJobs([int]$j) { return [int][Math]::Max(1, [Math]::Min($j - 1, [Math]::Ceiling($j / 2))) }

function Format-Duration([TimeSpan]$t) { return "{0:00}:{1:00}:{2:00}" -f [int][Math]::Floor($t.TotalHours), $t.Minutes, $t.Seconds }

$total = [System.Diagnostics.Stopwatch]::StartNew()
try {
    Write-Log "[vvl] Provisioning VK_LAYER_KHRONOS_validation ($Tag), jobs=$Jobs$(if($Force){', -Force'})" "Cyan"

    # --- 0. Fast path: already provisioned at this tag --------------------------------
    if (-not $Force -and (Test-Path $StampFile)) {
        $stampTag = (Get-Content $StampFile | Where-Object { $_ -like "tag=*" } | Select-Object -First 1) -replace '^tag=', ''
        $m = Find-Manifest
        if ($stampTag -eq $Tag -and $m -and (Test-Path (Join-Path $m.DirectoryName $LayerDll))) {
            Write-Log "[vvl] Already provisioned at $Tag (use -Force to rebuild)." "Green"
            Write-Log "[vvl] Manifest directory: $($m.DirectoryName)" "Green"
            Write-Output $m.DirectoryName
            return
        }
    }

    # --- 1. Verify the tag exists upstream ---------------------------------------------
    if (-not (Get-Command git -ErrorAction SilentlyContinue)) { throw "git not found on PATH." }
    Write-Log "[vvl] Checking that tag '$Tag' exists at $RepoUrl ..." "Cyan"
    $refs = Get-NativeOutput "git" @("ls-remote", "--tags", $RepoUrl)
    if ($script:LastNativeCode -ne 0) { throw "git ls-remote failed ($($script:LastNativeCode)); network problem?" }
    $plain  = $refs | Where-Object { $_ -match "^([0-9a-f]{40})\s+refs/tags/$([regex]::Escape($Tag))$" }  | Select-Object -First 1
    $peeled = $refs | Where-Object { $_ -match "^([0-9a-f]{40})\s+refs/tags/$([regex]::Escape($Tag))\^\{\}$" } | Select-Object -First 1
    if (-not $plain) { throw "Tag '$Tag' does not exist in $RepoUrl." }
    $tagCommit = (($(if ($peeled) { $peeled } else { $plain })) -split '\s+')[0]
    Write-Log "[vvl] Tag $Tag -> commit $tagCommit"

    # --- 2. Clone (shallow) or reuse ---------------------------------------------------
    $needClone = $true
    if (Test-Path "$SrcDir/.git") {
        $head = Get-NativeOutput "git" @("-C", $SrcDir, "rev-parse", "HEAD") | Select-Object -First 1
        if ($head -eq $tagCommit) {
            $needClone = $false
            Write-Log "[vvl] Reusing existing clone at $SrcDir ($head)."
        } else {
            Write-Log "[vvl] Existing clone is at '$head', expected $tagCommit; re-cloning." "Yellow"
        }
    }
    if ($needClone) {
        # A different tag means different pinned deps (external/) and build: start clean.
        foreach ($d in @($SrcDir, $InstallDir)) {
            if (Test-Path $d) { Write-Log "[vvl] Removing $d"; Remove-Item -Recurse -Force $d }
        }
        $code = Invoke-Native "git" @("-c", "advice.detachedHead=false", "clone", "--depth", "1", "--single-branch",
                                      "--branch", $Tag, $RepoUrl, $SrcDir)
        if ($code -ne 0) { throw "git clone failed ($code)." }
        $head = Get-NativeOutput "git" @("-C", $SrcDir, "rev-parse", "HEAD") | Select-Object -First 1
        if ($head -ne $tagCommit) { throw "Clone HEAD $head does not match tag commit $tagCommit." }
    }

    if ($Force) {
        foreach ($d in @($BuildDir, $InstallDir)) {
            if (Test-Path $d) { Write-Log "[vvl] -Force: removing $d"; Remove-Item -Recurse -Force $d }
        }
    }

    # --- 3. Toolchain: vcvars64 + bundled CMake/Ninja + Python 3 -----------------------
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found; is Visual Studio (Build Tools) installed?" }
    $vsPath = (& $vswhere -latest -products * -property installationPath | Select-Object -First 1)
    if (-not $vsPath) { throw "Could not resolve a Visual Studio installation path." }
    $vsPath   = $vsPath.Trim()
    $vcvars   = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
    $cmakeDir = Join-Path $vsPath "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
    $cmakeExe = Join-Path $cmakeDir "cmake.exe"
    $ninjaDir = Join-Path $vsPath "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"
    $ninjaExe = Join-Path $ninjaDir "ninja.exe"
    foreach ($p in @($vcvars, $cmakeExe, $ninjaExe)) {
        if (-not (Test-Path $p)) { throw "Required tool not found: $p" }
    }

    if ($env:VSCMD_ARG_TGT_ARCH -ne "x64") {
        Write-Log "[vvl] Importing MSVC x64 environment from vcvars64.bat ..." "Cyan"
        cmd /c "`"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
            if ($_ -match '^(.*?)=(.*)$') { Set-Item -Path "env:$($matches[1])" -Value $matches[2] }
        }
        if ($env:VSCMD_ARG_TGT_ARCH -ne "x64") { throw "vcvars64.bat did not produce an x64 environment." }
    }

    # Python 3 for update_deps.py. Avoid the Microsoft Store stub under WindowsApps.
    $pyCandidates = New-Object System.Collections.Generic.List[string]
    $pyCandidates.Add("C:\Python314\python.exe")
    if (Get-Command py.exe -ErrorAction SilentlyContinue) {
        Get-NativeOutput "py.exe" @("-3", "-c", "import sys; print(sys.executable)") | ForEach-Object { $pyCandidates.Add($_) }
    }
    foreach ($n in @("python.exe", "python3.exe")) {
        Get-Command $n -All -ErrorAction SilentlyContinue |
            Where-Object { $_.Source -notmatch '\\WindowsApps\\' } |
            ForEach-Object { $pyCandidates.Add($_.Source) }
    }
    $python = $null; $pyVersion = $null
    foreach ($c in $pyCandidates) {
        if (-not (Test-Path $c)) { continue }
        $v = Get-NativeOutput $c @("-c", "import sys; print('%d.%d' % sys.version_info[:2])") | Select-Object -First 1
        if ($script:LastNativeCode -eq 0 -and $v -and ([version]$v -ge [version]"3.8")) {
            $python = (Resolve-Path $c).Path; $pyVersion = [version]$v; break
        }
    }
    if (-not $python) { throw "No usable Python 3 found (needed by update_deps.py)." }
    if ($pyVersion -lt [version]"3.13") {
        Write-Log "[vvl] WARNING: Python $pyVersion ignores PYTHON_CPU_COUNT; dependency builds will use all cores." "Yellow"
    }

    $env:PATH = "$(Split-Path $python -Parent);$cmakeDir;$ninjaDir;$env:PATH"
    $env:VSLANG = "1033"   # English compiler diagnostics so the out-of-memory detection works
    Write-Log "[vvl] VS:     $vsPath"
    Write-Log "[vvl] CMake:  $cmakeExe"
    Write-Log "[vvl] Ninja:  $ninjaExe"
    Write-Log "[vvl] Python: $python ($pyVersion)"

    function Set-JobLimit([int]$j) {
        $env:PYTHON_CPU_COUNT = "$j"            # update_deps.py: cmake --build --parallel <cpu_count>
        $env:CMAKE_BUILD_PARALLEL_LEVEL = "$j"  # any other cmake --build without an explicit -j
    }

    # --- 4. Configure (runs update_deps.py: fetches + builds SPIRV-Tools, mimalloc, ...) -
    $configureArgs = @(
        "-S", $SrcDir, "-B", $BuildDir, "-G", "Ninja",
        "-DCMAKE_MAKE_PROGRAM=$($ninjaExe -replace '\\','/')",
        "-DCMAKE_BUILD_TYPE=Release",
        "-DCMAKE_INSTALL_PREFIX=$InstallDir",
        "-DUPDATE_DEPS=ON",
        "-DBUILD_TESTS=OFF",
        "-DBUILD_WERROR=OFF",
        "-DPython3_EXECUTABLE=$($python -replace '\\','/')"
    )
    # Resume support: keep dependencies that a previous (interrupted) run already installed.
    if (-not $Force) { $configureArgs += "-DUPDATE_DEPS_SKIP_EXISTING_INSTALL=ON" }

    $j = $Jobs
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    if (-not (Test-Path "$BuildDir/build.ninja")) {
        while ($true) {
            Set-JobLimit $j
            Write-Log "[vvl] Configuring (dependency builds with $j jobs) ..." "Cyan"
            $code = Invoke-Native $cmakeExe $configureArgs
            if ($code -eq 0 -and (Test-Path "$BuildDir/build.ninja")) { break }
            if ($script:SawOom -and $j -gt 1) {
                $j = Get-NextJobs $j
                Write-Log "[vvl] Out-of-memory detected during configure; retrying with $j jobs." "Yellow"
                if ($configureArgs -notcontains "-DUPDATE_DEPS_SKIP_EXISTING_INSTALL=ON") {
                    $configureArgs += "-DUPDATE_DEPS_SKIP_EXISTING_INSTALL=ON"
                }
                continue
            }
            throw "CMake configure failed ($code). See $LogFile"
        }
        Write-Log "[vvl] Configure done in $(Format-Duration $sw.Elapsed)." "Green"
    } else {
        Write-Log "[vvl] Build tree already configured: $BuildDir"
    }

    # --- 5. Build ------------------------------------------------------------------------
    $sw.Restart()
    while ($true) {
        Set-JobLimit $j
        Write-Log "[vvl] Building with $j jobs ..." "Cyan"
        $code = Invoke-Native $cmakeExe @("--build", $BuildDir, "--parallel", "$j")
        if ($code -eq 0) { break }
        if ($script:SawOom -and $j -gt 1) {
            $j = Get-NextJobs $j
            Write-Log "[vvl] Out-of-memory detected during build; retrying with $j jobs (Ninja resumes)." "Yellow"
            continue
        }
        throw "Build failed ($code). See $LogFile"
    }
    Write-Log "[vvl] Build done in $(Format-Duration $sw.Elapsed)." "Green"

    # --- 6. Install ----------------------------------------------------------------------
    $code = Invoke-Native $cmakeExe @("--install", $BuildDir)
    if ($code -ne 0) { throw "Install failed ($code). See $LogFile" }

    $m = Find-Manifest
    if (-not $m) { throw "Install finished but $ManifestName was not found under $InstallDir." }
    if (-not (Test-Path (Join-Path $m.DirectoryName $LayerDll))) {
        throw "$LayerDll is missing next to $($m.FullName)."
    }
    $manifestJson = Get-Content $m.FullName -Raw | ConvertFrom-Json
    if ($manifestJson.layer.name -ne "VK_LAYER_KHRONOS_validation") {
        throw "Unexpected layer name '$($manifestJson.layer.name)' in $($m.FullName)."
    }

    @(
        "tag=$Tag",
        "commit=$tagCommit",
        "api_version=$($manifestJson.layer.api_version)",
        "provisioned=$((Get-Date).ToString('s'))",
        "python=$python"
    ) | Set-Content -Path $StampFile -Encoding ASCII

    Write-Log "[vvl] Installed VK_LAYER_KHRONOS_validation (api $($manifestJson.layer.api_version), impl $($manifestJson.layer.implementation_version))." "Green"
    Write-Log "[vvl] Total time: $(Format-Duration $total.Elapsed)" "Green"
    Write-Log "[vvl] Manifest directory: $($m.DirectoryName)" "Green"
    Write-Output $m.DirectoryName
}
catch {
    Write-Log "[vvl] FAILED after $(Format-Duration $total.Elapsed): $($_.Exception.Message)" "Red"
    throw
}
finally {
    $script:LogWriter.Dispose()
}
