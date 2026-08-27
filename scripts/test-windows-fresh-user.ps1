<#
.SYNOPSIS
    WINDOWS FRESH-USER M5: proves an ordinary Windows x86_64 user - who has
    never touched this repository, MSYS2, LLVM, CMake, or Ninja - can
    download the KAI Windows ZIP, extract it, and compile/run real KAI
    programs after installing exactly ONE independently-obtained host C
    toolchain.

.DESCRIPTION
    Runnable both in GitHub Actions and by a maintainer on a real Windows
    x86_64 machine - this script contains no GitHub-Actions-only logic. It
    takes already-downloaded local files (the KAI Windows ZIP, a standalone
    host-toolchain ZIP, and optionally the win32-x64 VSIX + a portable VS
    Code ZIP) and never downloads anything itself, keeping the actual
    fresh-user smoke logic decoupled from network/CI concerns.

    Everything KAI-related (the extracted package, example sources, output
    executables, VS Code's isolated user-data/extensions dirs) runs from a
    FRESH directory under the Windows temp folder whose name deliberately
    contains a space, entirely outside this repository/any build tree/any
    MSYS2 installation.

    FIRST REAL WINDOWS RUN FINDING (CI PORTABILITY FIX): the first real
    Windows run found that the standalone WinLibs GCC distribution itself
    does not tolerate being relocated into an install prefix containing a
    space (ld.exe split the path at each space) - a limitation of that
    third-party toolchain, not of KAI. The host toolchain is therefore
    extracted to a SEPARATE, explicitly space-free location (see
    Get-SpaceFreeDirectory/-ToolchainWorkDir below); the KAI package
    itself, its examples, and every output executable still live under
    the space-containing fresh-user root - this script keeps proving KAI
    supports spaces, it just stops assuming the third-party toolchain
    does too.

    Every KAI process (kaicc.exe and the programs it compiles) is
    launched via System.Diagnostics.Process directly
    (UseShellExecute=$false, an argv list, never a shell) - the
    PowerShell equivalent of the extension's own shell-free
    spawnProcess(). The child's environment is constructed FRESH per
    invocation from a small, explicit allowlist (see
    New-SanitizedEnvironment below) - the running PowerShell session's own
    $env:PATH is never mutated. Every invocation also requires an
    EXPLICIT working directory inside the fresh-user tree (never the
    repository checkout) - see Invoke-Native's repo-cwd guard.

.PARAMETER KaiZipPath
    Path to the real, same-commit portable Windows ZIP
    (kai-<version>-windows-x86_64.zip) produced by
    scripts/build-release-windows-x86_64.sh.

.PARAMETER ToolchainZipPath
    Path to an independently-obtained, standalone Windows x86_64 GCC/Clang
    toolchain ZIP (NOT the KAI build's own MSYS2 UCRT64 environment - see
    this milestone's report for why WinLibs GCC was chosen).

.PARAMETER VsixPath
    Optional path to the same-commit win32-x64 VSIX
    (kai-language-win32-x64-<version>.vsix). If omitted, the VSIX install
    test is skipped and reported as a limitation, not silently ignored.

.PARAMETER VSCodeZipPath
    Optional path to a portable VS Code win32-x64 ZIP (see
    https://update.code.visualstudio.com/<version>/win32-x64-archive/stable).
    Required together with VsixPath for the real isolated VSIX install test.

.PARAMETER ExpectedVSCodeVersion
    Optional exact version string (e.g. "1.135.0") the resolved VS Code
    CLI's own `--version` output must report - the identity check that
    catches a WRONG cli.js being resolved (see this milestone's report: a
    real Windows run once silently ran @vscode/sandbox-runtime's own
    internal cli.js instead of the real one, which happily exited 0 and
    reported "1.0.0"). Strongly recommended whenever -VSCodeZipPath is
    given. Deliberately a parameter, not a literal hardcoded in this
    script: the script itself stays reusable against any VS Code archive a
    maintainer points it at; CI passes the real pinned version from
    scripts/windows-fresh-user-pins.json. If omitted, only a generic
    non-empty-version-line check is performed (no identity guarantee).

.PARAMETER WorkDir
    Optional override for the fresh, space-containing KAI test directory.
    If omitted, a unique directory containing a literal space is created
    under $env:TEMP.

.PARAMETER ToolchainWorkDir
    Optional override for where the standalone host toolchain is
    extracted. MUST NOT contain whitespace (validated - see
    Get-SpaceFreeDirectory). If omitted, a space-free directory is
    derived automatically from a small set of known-safe roots
    ($env:SystemDrive, $env:ProgramData, $env:TEMP), each checked
    explicitly rather than assumed.
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$KaiZipPath,
    [Parameter(Mandatory = $true)][string]$ToolchainZipPath,
    [string]$VsixPath = $null,
    [string]$VSCodeZipPath = $null,
    [string]$ExpectedVSCodeVersion = $null,
    [string]$WorkDir = $null,
    [string]$ToolchainWorkDir = $null
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:FailureCount = 0

# CI PORTABILITY FIX (repo-cwd guard, spec §7/§8): recorded BEFORE
# creating any fresh-user directory - every Invoke-Native call below is
# checked against this so a KAI user-operation can never silently
# inherit the repository checkout as its working directory again (the
# first real Windows run's secondary failure - "cannot find hello.exe" -
# surfaced only because the compile step had actually run inside
# D:\a\KAI-CC\KAI-CC, the GitHub Actions checkout).
$script:ForbiddenCwdRoots = @((Get-Location).Path)
if ($env:GITHUB_WORKSPACE) { $script:ForbiddenCwdRoots += $env:GITHUB_WORKSPACE }

function Write-Section([string]$Title) {
    Write-Host ''
    Write-Host "== $Title =="
}

function Assert-True([bool]$Condition, [string]$Message) {
    if ($Condition) {
        Write-Host "  PASS: $Message"
    } else {
        Write-Host "  FAIL: $Message"
        $script:FailureCount++
    }
}

function Assert-Equal([string]$Actual, [string]$Expected, [string]$Message) {
    if ($Actual -ceq $Expected) {
        Write-Host "  PASS: $Message"
    } else {
        Write-Host "  FAIL: $Message"
        Write-Host ("    expected ({0} chars): {1}" -f $Expected.Length, ($Expected -replace "`n", '\n'))
        Write-Host ("    actual   ({0} chars): {1}" -f $Actual.Length, ($Actual -replace "`n", '\n'))
        $script:FailureCount++
    }
}

# ---------------------------------------------------------------------
# Shell-free process invocation (the PowerShell equivalent of the VS Code
# extension's own process.ts spawnProcess(): UseShellExecute=$false, an
# ArgumentList collection - never a joined/quoted command string - so
# paths/arguments containing spaces are passed through exactly, and no
# cmd.exe/PowerShell shell metacharacter interpretation is ever possible).
# Captures RAW stdout/stderr bytes via the child's own redirected stream,
# never PowerShell's text pipeline (which can otherwise reinterpret line
# endings) - this is what lets Assert-Equal prove KAI's exact LF-only
# output byte-for-byte, with no normalization anywhere in this script.
#
# -WorkingDirectory is MANDATORY (CI PORTABILITY FIX, spec §7/§8): no
# call site can silently omit it and fall through to inheriting this
# script's own process cwd. It is additionally checked against
# $script:ForbiddenCwdRoots - the repository checkout (and
# $env:GITHUB_WORKSPACE, when set) - and rejected immediately if it
# matches, so a KAI user-operation running inside the checkout directory
# is a hard error here, not a silent correctness bug discovered later.
# ---------------------------------------------------------------------
function Invoke-Native {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [string[]]$ArgumentList = @(),
        [Parameter(Mandatory = $true)][string]$WorkingDirectory,
        [Parameter(Mandatory = $true)]$Sanitized,
        [hashtable]$ExtraEnv = @{}
    )

    foreach ($forbidden in $script:ForbiddenCwdRoots) {
        if ($WorkingDirectory.StartsWith($forbidden, [System.StringComparison]::OrdinalIgnoreCase)) {
            throw "Invoke-Native: refusing to use a working directory under the repository checkout ($forbidden): $WorkingDirectory"
        }
    }
    if (-not (Test-Path -LiteralPath $WorkingDirectory -PathType Container)) {
        throw "Invoke-Native: working directory does not exist: $WorkingDirectory"
    }

    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $FilePath
    foreach ($a in $ArgumentList) { [void]$psi.ArgumentList.Add($a) }
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.CreateNoWindow = $true
    $psi.WorkingDirectory = $WorkingDirectory

    # ProcessStartInfo.Environment/EnvironmentVariables is pre-populated
    # (by .NET) as a COPY of this PowerShell process's own environment.
    # Deliberately do NOT try to selectively delete unwanted inherited
    # variables from that copy: a local test against the real .NET API
    # showed that approach can leave a stale variable visible to the
    # child even after Remove() reports success. Instead, clear the WHOLE
    # inherited environment first and rebuild it from Sanitized.Vars (an
    # explicit allowlist - see New-SanitizedEnvironment) - this is a
    # strictly stronger guarantee: nothing can leak through that wasn't
    # explicitly added back. $env:PATH (and every other real variable in
    # THIS PowerShell session) is never touched - only the copy handed to
    # each child process.
    $psi.EnvironmentVariables.Clear()
    foreach ($key in $Sanitized.Vars.Keys) { $psi.Environment[$key] = $Sanitized.Vars[$key] }
    foreach ($key in $ExtraEnv.Keys) { $psi.Environment[$key] = $ExtraEnv[$key] }

    $proc = [System.Diagnostics.Process]::Start($psi)
    $stdoutMs = New-Object System.IO.MemoryStream
    $stderrMs = New-Object System.IO.MemoryStream
    $stdoutCopy = $proc.StandardOutput.BaseStream.CopyToAsync($stdoutMs)
    $stderrCopy = $proc.StandardError.BaseStream.CopyToAsync($stderrMs)
    $proc.WaitForExit()
    $stdoutCopy.Wait()
    $stderrCopy.Wait()

    $stdoutBytes = $stdoutMs.ToArray()
    $stderrBytes = $stderrMs.ToArray()

    [PSCustomObject]@{
        ExitCode    = $proc.ExitCode
        StdoutBytes = $stdoutBytes
        StderrBytes = $stderrBytes
        Stdout      = [System.Text.Encoding]::UTF8.GetString($stdoutBytes)
        Stderr      = [System.Text.Encoding]::UTF8.GetString($stderrBytes)
    }
}

# ---------------------------------------------------------------------
# Sanitized environment (spec §7/§32/§33): an explicit ALLOWLIST, never a
# denylist of things to strip out of whatever the host happens to have -
# Invoke-Native clears the child's ENTIRE inherited environment first
# (see its own comment for why), then adds back exactly what's listed
# here. This is what actually guarantees KAI_CC/KAI_RUNTIME_LIB/LLVM_DIR/
# MSYSTEM/etc. (and the MSYS2 UCRT64/LLVM/CMake/Ninja/repository-build
# directories on PATH) can never leak into the fresh-user process,
# regardless of what the CI runner or a maintainer's own dev shell
# happens to have set - there is no scenario where an unlisted variable
# survives, because nothing survives by default. UNCHANGED by the CI
# portability fix - the first real Windows run already proved this
# design works; the WinLibs relocation limitation is a separate concern
# handled entirely via WHERE the toolchain is extracted (see
# Get-SpaceFreeDirectory), never by weakening this allowlist.
#
# The PATH itself contains only the handful of Windows system
# directories an ordinary process needs to start at all, plus - once
# explicitly requested - the independently-obtained host toolchain's own
# bin directory. Never MSYS2 UCRT64/LLVM/CMake/Ninja/a repository build
# dir. Derived from $env:SystemRoot/$env:TEMP (never a hardcoded
# "C:\Windows"). A few other genuinely ordinary Windows environment
# variables (SystemRoot/windir, TEMP/TMP, ComSpec, PATHEXT) are included
# too - real Windows programs (a compiler needing a temp directory for
# intermediate files, the loader consulting %SystemRoot%) commonly expect
# these to exist; omitting them would risk a FALSE failure that looks
# like a portability bug but is really just an unrealistically broken
# environment no real Windows user would ever have.
# ---------------------------------------------------------------------
function New-SanitizedEnvironment {
    param([string]$ToolchainBinDir = $null)

    $systemRoot = $env:SystemRoot
    if (-not $systemRoot) { throw 'SystemRoot is not set - cannot build a sanitized environment.' }

    $pathParts = @(
        (Join-Path $systemRoot 'System32'),
        $systemRoot,
        (Join-Path $systemRoot 'System32/Wbem'),
        (Join-Path $systemRoot 'System32/WindowsPowerShell/v1.0')
    )
    if ($ToolchainBinDir) { $pathParts += $ToolchainBinDir }

    [PSCustomObject]@{
        Vars = @{
            Path       = [string]::Join(';', $pathParts)
            SystemRoot = $systemRoot
            windir     = $systemRoot
            TEMP       = $env:TEMP
            TMP        = $env:TMP
            ComSpec    = (Join-Path $systemRoot 'System32/cmd.exe')
            PATHEXT    = '.COM;.EXE;.BAT;.CMD'
        }
    }
}

# Returns { Absent: bool; ResolvedPaths: string[] } - `where.exe` can print
# more than one match (one per line), so every match is captured, not just
# whether it succeeded.
function Test-CommandResolution([string]$CommandName, $Sanitized, [string]$WorkingDirectory) {
    $whereExe = Join-Path $env:SystemRoot 'System32/where.exe'
    $result = Invoke-Native -FilePath $whereExe -ArgumentList @($CommandName) -WorkingDirectory $WorkingDirectory -Sanitized $Sanitized
    if ($result.ExitCode -ne 0) {
        return [PSCustomObject]@{ Absent = $true; ResolvedPaths = @() }
    }
    $paths = @($result.Stdout -split "`r?`n" | Where-Object { $_.Trim().Length -gt 0 })
    return [PSCustomObject]@{ Absent = $false; ResolvedPaths = $paths }
}

function Test-CommandAbsent([string]$CommandName, $Sanitized, [string]$WorkingDirectory) {
    return (Test-CommandResolution -CommandName $CommandName -Sanitized $Sanitized -WorkingDirectory $WorkingDirectory).Absent
}

# ---------------------------------------------------------------------
# BASH ABSENCE CHECK CLASSIFICATION FIX: the real release-gate invariant
# is "KAI must not be accidentally using the MSYS2/MinGW development
# environment that built it" - NOT "no executable named bash may exist
# anywhere on the machine". A real Windows run found `bash` DOES resolve
# under the sanitized environment (System32 is - correctly - part of the
# sanitized PATH allowlist, and Windows installations with WSL enabled,
# including GitHub's own hosted runners, place a `bash.exe` WSL-launcher
# stub directly under %SystemRoot%\System32) - that is Windows/system-
# provided tooling entirely unrelated to KAI's own build environment, not
# evidence of MSYS2 leakage. This checks the RESOLVED PATH itself against
# the same dev-environment name patterns already used elsewhere in this
# script (msys64/ucrt64/mingw64/mingw32), never the mere fact that a
# command named "bash" resolves to something.
# ---------------------------------------------------------------------
function Test-PathIsForbiddenDevEnvironment([string]$Path) {
    return ($Path -match '(?i)msys64|ucrt64|mingw64|mingw32')
}

function Find-FileRecursive([string]$Root, [string]$Name) {
    $found = Get-ChildItem -Path $Root -Filter $Name -Recurse -File -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $found) { return $null }
    return $found.FullName
}

# ---------------------------------------------------------------------
# EXACT VS CODE CLI ENTRYPOINT RESOLUTION: a real Windows run found that
# a loose "**\cli.js" recursive filename search (Find-FileRecursive) can
# select the WRONG file - the pinned VS Code 1.135.0 archive contains
# THREE files literally named cli.js:
#   <build-id>/resources/app/out/cli.js                                 <- the real one
#   <build-id>/resources/app/node_modules/@vscode/sandbox-runtime/dist/cli.js  <- an unrelated internal tool
#   <build-id>/resources/app/node_modules.asar.unpacked/.../codegen-cli.js    <- unrelated (different name, listed for completeness)
# and filesystem enumeration order happened to return the sandbox-runtime
# one first, which happily started, reported its own "1.0.0", and then
# failed --install-extension with "Sandbox dependencies not available".
#
# code.cmd itself (inspected directly from the pinned archive - see the
# previous round's report) is unambiguous about the REAL entrypoint's
# STRUCTURAL shape: "%~dp0..\<build-id>\resources\app\out\cli.js" - i.e.
# exactly one path of the form <VSCodeRoot>\<single top-level
# directory>\resources\app\out\cli.js. This function checks ONLY that
# exact structural shape (one Join-Path per top-level directory, never a
# recursive filename search), which categorically excludes anything
# nested under node_modules/sandbox-runtime/or any other internal tool -
# there is no loose "first match wins" here. Zero matches or more than
# one match are both treated as failures, never resolved by guessing.
# ---------------------------------------------------------------------
function Find-VSCodeCliJs {
    param([Parameter(Mandatory = $true)][string]$VSCodeRoot)

    $candidates = @(
        Get-ChildItem -Path $VSCodeRoot -Directory -ErrorAction SilentlyContinue |
            ForEach-Object { Join-Path $_.FullName 'resources/app/out/cli.js' } |
            Where-Object { Test-Path -LiteralPath $_ -PathType Leaf }
    )

    if ($candidates.Count -eq 0) {
        throw "Find-VSCodeCliJs: no '<build-id>/resources/app/out/cli.js' found directly under $VSCodeRoot"
    }
    if ($candidates.Count -gt 1) {
        throw "Find-VSCodeCliJs: ambiguous - found $($candidates.Count) '<build-id>/resources/app/out/cli.js' candidates under ${VSCodeRoot}: $($candidates -join '; ')"
    }
    return $candidates[0]
}

# ---------------------------------------------------------------------
# CI PORTABILITY FIX §3: constructs (or validates) a directory GUARANTEED
# not to contain whitespace, for the standalone host toolchain only -
# never assumed, always checked explicitly. Tries a small set of
# well-known roots that are extremely unlikely to contain spaces
# (%SystemDrive%\, %ProgramData%, then %TEMP% as a last resort) rather
# than blindly trusting any single one - a real user's own profile
# directory name (e.g. "C:\Users\John Doe\...") could otherwise
# reintroduce exactly the bug this fix exists to avoid. Fails clearly if
# none of them can be established as space-free, rather than silently
# proceeding into the same failure mode observed on the first real
# Windows run.
# ---------------------------------------------------------------------
function Get-SpaceFreeDirectory {
    param([string]$PreferredPath = $null)

    if ($PreferredPath) {
        if ($PreferredPath -match '\s') {
            throw "-ToolchainWorkDir must not contain whitespace: $PreferredPath"
        }
        New-Item -ItemType Directory -Path $PreferredPath -Force | Out-Null
        return (Resolve-Path -LiteralPath $PreferredPath).Path
    }

    $candidateRoots = @()
    if (-not [string]::IsNullOrWhiteSpace($env:SystemDrive)) { $candidateRoots += ($env:SystemDrive + '\') }
    if (-not [string]::IsNullOrWhiteSpace($env:ProgramData)) { $candidateRoots += $env:ProgramData }
    if (-not [string]::IsNullOrWhiteSpace($env:TEMP)) { $candidateRoots += $env:TEMP }
    foreach ($root in $candidateRoots) {
        if ($root -match '\s') { continue }
        $suffix = [guid]::NewGuid().ToString('N').Substring(0, 8)
        $candidate = Join-Path $root "kai-fresh-user-toolchain-$suffix"
        if ($candidate -notmatch '\s') {
            New-Item -ItemType Directory -Path $candidate -Force | Out-Null
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    throw 'Get-SpaceFreeDirectory: could not construct a space-free directory for the host toolchain from any known root (SystemDrive/ProgramData/TEMP) - pass -ToolchainWorkDir explicitly.'
}

# ---------------------------------------------------------------------
# CI PORTABILITY FIX §5: compiles a trivial C program through the given
# gcc.exe - used both as the root-cause A/B experiment (spaced vs
# space-free toolchain prefix) and as the mandatory host-toolchain
# preflight before any real KAI compilation. Deliberately compiles/links
# from a caller-supplied WorkingDirectory (which MAY itself contain a
# space, proving that only the TOOLCHAIN's own install prefix needs to
# be space-free - its input/output/cwd do not).
# ---------------------------------------------------------------------
function Test-TrivialCCompile {
    param(
        [Parameter(Mandatory = $true)][string]$GccPath,
        [Parameter(Mandatory = $true)][string]$WorkingDirectory,
        [Parameter(Mandatory = $true)]$Sanitized
    )
    $srcPath = Join-Path $WorkingDirectory 'kai_preflight.c'
    $outputPath = Join-Path $WorkingDirectory 'kai_preflight'
    $exePath = "$outputPath.exe"
    Remove-Item -LiteralPath $exePath -ErrorAction SilentlyContinue
    Set-Content -LiteralPath $srcPath -Value 'int main(void) { return 0; }' -NoNewline

    $result = Invoke-Native -FilePath $GccPath -ArgumentList @($srcPath, '-o', $outputPath) -WorkingDirectory $WorkingDirectory -Sanitized $Sanitized
    $success = ($result.ExitCode -eq 0) -and (Test-Path -LiteralPath $exePath -PathType Leaf)

    Remove-Item -LiteralPath $srcPath, $exePath -ErrorAction SilentlyContinue

    [PSCustomObject]@{ Success = $success; ExitCode = $result.ExitCode; Stderr = $result.Stderr }
}

# Prints at most $MaxChars of a captured stream, never the whole thing
# unbounded (spec §1: "keep logs bounded").
function Write-BoundedOutput([string]$Label, [string]$Text, [int]$MaxChars = 2000) {
    if ([string]::IsNullOrEmpty($Text)) {
        Write-Host "    ${Label}: (empty)"
        return
    }
    $shown = $Text.Trim()
    if ($shown.Length -gt $MaxChars) { $shown = $shown.Substring(0, $MaxChars) + '... [truncated]' }
    Write-Host "    ${Label}: $shown"
}

# ---------------------------------------------------------------------
# VS CODE CLI ISOLATED-INSTALL DIAGNOSIS: reads extension/package.json
# directly out of the VSIX (an ordinary zip) to determine the exact
# publisher/name/version this specific artifact declares, and the exact
# "publisher.name@version" identifier `--list-extensions --show-versions`
# is expected to print - never a hardcoded/guessed string, and never a
# repack of the VSIX itself. Also records extension.vsixmanifest's
# TargetPlatform attribute when present (best-effort - the install test
# itself is the authoritative proof, this is corroborating evidence).
# ---------------------------------------------------------------------
function Test-VsixStructure {
    param([Parameter(Mandatory = $true)][string]$VsixPath)

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [System.IO.Compression.ZipFile]::OpenRead($VsixPath)
    try {
        $packageJsonEntry = $zip.Entries | Where-Object { $_.FullName -eq 'extension/package.json' }
        if (-not $packageJsonEntry) {
            throw "VSIX structural check: 'extension/package.json' not found inside $VsixPath"
        }
        $reader = New-Object System.IO.StreamReader($packageJsonEntry.Open())
        try { $manifest = $reader.ReadToEnd() | ConvertFrom-Json } finally { $reader.Dispose() }

        $targetPlatform = $null
        $vsixManifestEntry = $zip.Entries | Where-Object { $_.FullName -eq 'extension.vsixmanifest' }
        if ($vsixManifestEntry) {
            $manifestReader = New-Object System.IO.StreamReader($vsixManifestEntry.Open())
            try {
                [xml]$vsixManifestXml = $manifestReader.ReadToEnd()
                $identity = $vsixManifestXml.PackageManifest.Metadata.Identity
                if ($identity -and $identity.TargetPlatform) { $targetPlatform = $identity.TargetPlatform }
            } finally { $manifestReader.Dispose() }
        }
    } finally {
        $zip.Dispose()
    }

    [PSCustomObject]@{
        Publisher      = $manifest.publisher
        Name           = $manifest.name
        Version        = $manifest.version
        ExpectedId     = "$($manifest.publisher).$($manifest.name)@$($manifest.version)"
        TargetPlatform = $targetPlatform
    }
}

# ---------------------------------------------------------------------
# VS CODE CLI ISOLATED-INSTALL DIAGNOSIS: a VS Code-SPECIFIC sanitized
# environment, built from the SAME base allowlist as KAI's own
# (New-SanitizedEnvironment - no toolchain, no MSYS2/LLVM/CMake/Ninja),
# plus exactly the additional variables inspecting the pinned VS Code
# archive's own bin/code.cmd showed it needs, and a small set of
# ordinary Windows user-profile variables (USERPROFILE/APPDATA/
# LOCALAPPDATA/HOMEDRIVE/HOMEPATH) VS Code's own Electron/Node runtime
# expects to exist even when --user-data-dir/--extensions-dir are given
# explicitly. Every one of those points at a FRESH, isolated directory
# under this invocation's own fresh-user tree - never the real
# runner/maintainer profile, never reused across runs.
# ---------------------------------------------------------------------
function New-VSCodeSanitizedEnvironment {
    param([Parameter(Mandatory = $true)][string]$IsolatedProfileRoot)

    $userProfile = Join-Path $IsolatedProfileRoot 'profile'
    $appDataRoaming = Join-Path $userProfile 'AppData/Roaming'
    $appDataLocal = Join-Path $userProfile 'AppData/Local'
    New-Item -ItemType Directory -Path $userProfile, $appDataRoaming, $appDataLocal -Force | Out-Null

    $vars = (New-SanitizedEnvironment).Vars.Clone()
    # bin/code.cmd's own contents (inspected directly from the pinned
    # archive - see this milestone's report): sets exactly these two
    # before invoking Code.exe as a plain Node process on its cli.js.
    $vars['ELECTRON_RUN_AS_NODE'] = '1'
    $vars['VSCODE_DEV'] = ''
    # Ordinary, isolated Windows user-profile variables - not part of
    # KAI's own compiler environment, added here only because VS Code's
    # runtime needs them to start at all.
    $vars['USERPROFILE'] = $userProfile
    $vars['APPDATA'] = $appDataRoaming
    $vars['LOCALAPPDATA'] = $appDataLocal
    $vars['HOMEDRIVE'] = $env:SystemDrive
    $vars['HOMEPATH'] = $userProfile.Substring(2)

    [PSCustomObject]@{ Vars = $vars }
}

# ======================================================================
# 0. Validate inputs up front - clear, specific failures (spec §40)
# ======================================================================
Write-Section 'Validating inputs'
if (-not (Test-Path -LiteralPath $KaiZipPath -PathType Leaf)) {
    throw "KAI Windows ZIP not found at: $KaiZipPath"
}
if (-not (Test-Path -LiteralPath $ToolchainZipPath -PathType Leaf)) {
    throw "Host toolchain ZIP not found at: $ToolchainZipPath"
}
$kaiZipSizeMb = [math]::Round((Get-Item -LiteralPath $KaiZipPath).Length / 1MB, 2)
Write-Host "  KAI Windows ZIP: $KaiZipPath ($kaiZipSizeMb MB)"
Write-Host "  Host toolchain ZIP: $ToolchainZipPath"
if ($VsixPath) {
    if (-not (Test-Path -LiteralPath $VsixPath -PathType Leaf)) {
        throw "VSIX not found at: $VsixPath"
    }
    $vsixSizeMb = [math]::Round((Get-Item -LiteralPath $VsixPath).Length / 1MB, 2)
    Write-Host "  VSIX: $VsixPath ($vsixSizeMb MB)"
}

# ======================================================================
# 1. Fresh KAI directory OUTSIDE the repo/build tree/MSYS2, path with a
#    space (spec §4/§19/§20) - this is the tree that MUST keep proving
#    KAI supports spaces; only the host toolchain (below) moves elsewhere.
# ======================================================================
Write-Section 'Creating fresh KAI test directory (outside repo/build/MSYS2, path contains a space)'
if (-not $WorkDir) {
    $suffix = [guid]::NewGuid().ToString('N').Substring(0, 8)
    $WorkDir = Join-Path $env:TEMP "KAI Fresh User $suffix"
}
New-Item -ItemType Directory -Path $WorkDir -Force | Out-Null
Write-Host "  WorkDir: $WorkDir"
Assert-True ($WorkDir -match ' ') 'KAI fresh-user root contains a space'
foreach ($forbidden in $script:ForbiddenCwdRoots) {
    Assert-True (-not $WorkDir.StartsWith($forbidden, [System.StringComparison]::OrdinalIgnoreCase)) "KAI fresh-user root is not under the repository checkout ($forbidden)"
}

# ======================================================================
# 2. Extract the KAI ZIP using ordinary Windows facilities (spec §6)
# ======================================================================
Write-Section 'Extracting the KAI Windows ZIP (Expand-Archive)'
$kaiExtractRoot = Join-Path $WorkDir 'kai-zip'
Expand-Archive -LiteralPath $KaiZipPath -DestinationPath $kaiExtractRoot -Force
$kaiRoot = Join-Path $kaiExtractRoot 'kai-windows-x86_64'
Assert-True (Test-Path -LiteralPath $kaiRoot -PathType Container) 'archive root kai-windows-x86_64/ exists after extraction'
$examplesDir = Join-Path $kaiRoot 'examples'

$kaiccExe = Join-Path $kaiRoot 'bin/kaicc.exe'
$runtimeArchive = Join-Path $kaiRoot 'lib/kai/libkai_runtime.a'
$requiredDlls = @('libgcc_s_seh-1.dll', 'libstdc++-6.dll', 'libwinpthread-1.dll', 'zlib1.dll', 'libzstd.dll')

foreach ($check in @(
        @{ Path = $kaiccExe; Label = 'bin/kaicc.exe' },
        @{ Path = $runtimeArchive; Label = 'lib/kai/libkai_runtime.a' },
        @{ Path = (Join-Path $kaiRoot 'LICENSE'); Label = 'LICENSE' },
        @{ Path = (Join-Path $kaiRoot 'THIRD_PARTY_NOTICES.md'); Label = 'THIRD_PARTY_NOTICES.md' },
        @{ Path = (Join-Path $examplesDir 'hello.kai'); Label = 'examples/hello.kai' },
        @{ Path = (Join-Path $examplesDir 'functions.kai'); Label = 'examples/functions.kai' },
        @{ Path = (Join-Path $examplesDir 'conditions.kai'); Label = 'examples/conditions.kai' },
        @{ Path = (Join-Path $examplesDir 'variables.kai'); Label = 'examples/variables.kai' }
    )) {
    if (-not (Test-Path -LiteralPath $check.Path -PathType Leaf)) {
        throw "malformed KAI release archive: expected file missing: $($check.Label)"
    }
}
foreach ($dll in $requiredDlls) {
    $dllPath = Join-Path $kaiRoot "bin/$dll"
    if (-not (Test-Path -LiteralPath $dllPath -PathType Leaf)) {
        throw "malformed KAI release archive: required DLL missing: bin\$dll"
    }
}
Write-Host '  archive layout OK: kaicc.exe, libkai_runtime.a, 5 DLLs, legal files, curated examples all present'

# ======================================================================
# 3. Root-cause investigation (CI PORTABILITY FIX spec §1): does the
#    standalone WinLibs toolchain tolerate a space-containing install
#    prefix, or not? Reproduced directly with gcc.exe - no KAI involved
#    at all - BEFORE trusting the space-free relocation "fix" below.
# ======================================================================
Write-Section 'Investigation: does the host toolchain tolerate a space-containing install prefix?'
$diagnosticSpacedRoot = Join-Path $WorkDir 'Toolchain Diagnostic With Spaces'
Expand-Archive -LiteralPath $ToolchainZipPath -DestinationPath $diagnosticSpacedRoot -Force
$diagnosticSpacedGcc = Find-FileRecursive -Root $diagnosticSpacedRoot -Name 'gcc.exe'
if (-not $diagnosticSpacedGcc) {
    throw 'invalid host toolchain archive: gcc.exe not found anywhere under the extracted tree (spaced-prefix diagnostic copy)'
}

$toolchainWorkDir = Get-SpaceFreeDirectory -PreferredPath $ToolchainWorkDir
Expand-Archive -LiteralPath $ToolchainZipPath -DestinationPath $toolchainWorkDir -Force
$gccPath = Find-FileRecursive -Root $toolchainWorkDir -Name 'gcc.exe'
if (-not $gccPath) {
    throw 'invalid host toolchain archive: gcc.exe not found anywhere under the extracted tree (space-free copy)'
}
$toolchainBinDir = Split-Path -Parent $gccPath
Write-Host "  gcc.exe (space-free copy) found at: $gccPath"
Assert-True ($toolchainBinDir -notlike '*msys64*' -and $toolchainBinDir -notlike '*ucrt64*') `
    'host toolchain path does not come from an MSYS2 tree (genuinely independent)'
Assert-True ($toolchainWorkDir -notmatch '\s') 'host toolchain install prefix contains no whitespace'

# Both diagnostic compiles deliberately run from the SAME (space-
# containing) working directory - the only variable being isolated here
# is the toolchain's OWN install prefix, not where it compiles/links.
$diagnosticWorkArea = Join-Path $WorkDir 'preflight area'
New-Item -ItemType Directory -Path $diagnosticWorkArea -Force | Out-Null
$sanitizedForDiagnostic = New-SanitizedEnvironment -ToolchainBinDir $toolchainBinDir

$spacedResult = Test-TrivialCCompile -GccPath $diagnosticSpacedGcc -WorkingDirectory $diagnosticWorkArea -Sanitized $sanitizedForDiagnostic
Write-Host "  direct gcc compile, toolchain prefix CONTAINS a space: $(if ($spacedResult.Success) { 'SUCCESS' } else { 'FAILED' })"
if (-not $spacedResult.Success) { Write-Host "    stderr: $($spacedResult.Stderr.Trim())" }

Write-Section 'Host toolchain preflight (self-compile check, space-free prefix - spec §5)'
$freeResult = Test-TrivialCCompile -GccPath $gccPath -WorkingDirectory $diagnosticWorkArea -Sanitized $sanitizedForDiagnostic
Write-Host "  direct gcc compile, toolchain prefix is space-free: $(if ($freeResult.Success) { 'SUCCESS' } else { 'FAILED' })"
if (-not $freeResult.Success) { Write-Host "    stderr: $($freeResult.Stderr.Trim())" }

Remove-Item -LiteralPath $diagnosticSpacedRoot -Recurse -Force -ErrorAction SilentlyContinue

if ($spacedResult.Success) {
    Write-Host ''
    Write-Host 'UNEXPECTED: the standalone host toolchain compiled successfully even from a'
    Write-Host 'space-containing install prefix. This contradicts the working hypothesis from'
    Write-Host 'the first real Windows run (ld.exe path-splitting on the toolchain''s own'
    Write-Host 'prefix). STOPPING here rather than assuming the space-prefix explanation -'
    Write-Host 'the earlier failure needs further investigation before trusting this fix.'
    exit 1
}
if (-not $freeResult.Success) {
    throw "Selected host toolchain is not self-functional in its current installation path/environment, even from a space-free prefix - this is a DIFFERENT problem than the one previously observed. stderr: $($freeResult.Stderr.Trim())"
}
Write-Host ''
Write-Host '  CONFIRMED: the standalone host toolchain requires a space-free install prefix'
Write-Host '  (this is the root cause of the first real Windows run''s failure - a limitation'
Write-Host '  of the third-party toolchain distribution, not of KAI).'

# ======================================================================
# 4. Sanitized environment WITHOUT the host toolchain yet (spec §7/§8)
# ======================================================================
$sanitizedNoToolchain = New-SanitizedEnvironment
Write-Section 'Dev-tool absence check (sanitized PATH, before adding the host toolchain)'
# These have no legitimate Windows-system-provided identity at all - any
# resolution of them under the sanitized PATH would itself be suspicious,
# so the strict "must not resolve" check remains exactly as before.
foreach ($cmd in @('llvm-config', 'cmake', 'ninja', 'clang', 'gcc', 'cc')) {
    Assert-True (Test-CommandAbsent -CommandName $cmd -Sanitized $sanitizedNoToolchain -WorkingDirectory $kaiRoot) "'$cmd' is NOT resolvable in the sanitized environment"
}

# 'bash' is classified, not blindly rejected (see Test-PathIsForbiddenDevEnvironment's
# own comment): Windows itself can legitimately provide a bash.exe (the
# WSL launcher stub under %SystemRoot%\System32 - present on GitHub's own
# hosted Windows runners) that has nothing to do with KAI's own MSYS2/
# MinGW build environment. Only fail if a resolved path actually points
# into that forbidden dev-environment tree.
$bashCheck = Test-CommandResolution -CommandName 'bash' -Sanitized $sanitizedNoToolchain -WorkingDirectory $kaiRoot
if ($bashCheck.Absent) {
    Write-Host '  bash: not resolvable in the sanitized environment'
} else {
    foreach ($p in $bashCheck.ResolvedPaths) { Write-Host "  bash resolved to: $p" }
}
$forbiddenBashPaths = @($bashCheck.ResolvedPaths | Where-Object { Test-PathIsForbiddenDevEnvironment $_ })
Assert-True ($forbiddenBashPaths.Count -eq 0) 'resolved bash (if any) is not from KAI''s own MSYS2/MinGW development environment'

Assert-True ($sanitizedNoToolchain.Vars.Path -notmatch '(?i)mingw|ucrt64|msys64') 'sanitized PATH contains no MSYS2/MinGW dev-environment directory'

# ======================================================================
# 5. kaicc.exe starts and answers semantic queries with NO host linker
#    exposed at all (spec §9/§10/§21 - the most important portability
#    statement this milestone makes). All run with cwd=$kaiRoot - inside
#    the extracted package, never the repository checkout.
# ======================================================================
Write-Section 'kaicc.exe startup WITHOUT any host toolchain on PATH'
$versionResult = Invoke-Native -FilePath $kaiccExe -ArgumentList @('--version') -WorkingDirectory $kaiRoot -Sanitized $sanitizedNoToolchain
Assert-True ($versionResult.ExitCode -eq 0) 'kaicc.exe --version exits 0 with no host toolchain present'
Assert-True ($versionResult.Stdout -match '^KAI-CC 0\.1\.0-alpha\.1') "kaicc.exe --version reports the expected 'KAI-CC 0.1.0-alpha.1' (got: $($versionResult.Stdout.Trim()))"

$helpResult = Invoke-Native -FilePath $kaiccExe -ArgumentList @('--help') -WorkingDirectory $kaiRoot -Sanitized $sanitizedNoToolchain
Assert-True ($helpResult.ExitCode -eq 0) 'kaicc.exe --help exits 0 with no host toolchain present'
Assert-True ($helpResult.Stdout.Length -gt 0) 'kaicc.exe --help produced non-empty output'

Write-Section 'Semantic tooling WITHOUT any host linker (no native link needed)'
$functionsKai = Join-Path $examplesDir 'functions.kai'
$inspectResult = Invoke-Native -FilePath $kaiccExe -ArgumentList @('inspect', $functionsKai, '--json') -WorkingDirectory $kaiRoot -Sanitized $sanitizedNoToolchain
Assert-True ($inspectResult.ExitCode -eq 0 -and $inspectResult.Stdout -match '"kind":"function"') 'kaicc.exe inspect works without a host linker'

$referencesResult = Invoke-Native -FilePath $kaiccExe -ArgumentList @('references', $functionsKai, '--line', '15', '--column', '13', '--json') -WorkingDirectory $kaiRoot -Sanitized $sanitizedNoToolchain
Assert-True ($referencesResult.ExitCode -eq 0 -and $referencesResult.Stdout -match '"line"') 'kaicc.exe references works without a host linker'

$callGraphResult = Invoke-Native -FilePath $kaiccExe -ArgumentList @('call-graph', $functionsKai, '--json') -WorkingDirectory $kaiRoot -Sanitized $sanitizedNoToolchain
Assert-True ($callGraphResult.ExitCode -eq 0 -and $callGraphResult.Stdout -match '"add"') 'kaicc.exe call-graph works without a host linker'

# ======================================================================
# 6. Missing-linker UX (spec §22) - what a first-time Windows user sees
#    before installing any toolchain at all
# ======================================================================
Write-Section 'Missing-linker diagnostic (deliberately no toolchain on PATH)'
$helloKai = Join-Path $examplesDir 'hello.kai'
$missingLinkerOut = Join-Path $WorkDir 'hello-no-linker'
$missingLinkerResult = Invoke-Native -FilePath $kaiccExe -ArgumentList @($helloKai, '-o', $missingLinkerOut) -WorkingDirectory $kaiRoot -Sanitized $sanitizedNoToolchain
Assert-True ($missingLinkerResult.ExitCode -eq 9) "compiling without a host linker fails with the documented exit code 9 (got $($missingLinkerResult.ExitCode))"
Assert-True ($missingLinkerResult.Stderr -match 'no usable host C compiler driver') 'missing-linker stderr names the real problem clearly'
Assert-True ($missingLinkerResult.Stderr -notmatch [regex]::Escape($WorkDir) ) 'missing-linker stderr does not leak the fresh-user temp path'
Assert-True (-not (Test-Path -LiteralPath "$missingLinkerOut.exe")) 'no partial/stale executable was left behind on failure'

# ======================================================================
# 7. Add ONLY the independently-obtained host toolchain to PATH (spec
#    §13) and prove DEFAULT discovery (no KAI_CC) works
# ======================================================================
$sanitizedWithToolchain = New-SanitizedEnvironment -ToolchainBinDir $toolchainBinDir
Write-Section 'Standalone host toolchain now on PATH'
$gccVersionResult = Invoke-Native -FilePath $gccPath -ArgumentList @('--version') -WorkingDirectory $kaiRoot -Sanitized $sanitizedWithToolchain
$gccVersionLine = ($gccVersionResult.Stdout -split "`n")[0].Trim()
Write-Host "  selected linker: $gccPath"
Write-Host "  selected linker version: $gccVersionLine"

# CI PORTABILITY FIX spec §6: compile and run are now two clearly
# separate phases - if compile does not genuinely succeed (exit 0 AND
# the .exe actually exists), the function returns immediately WITHOUT
# attempting to launch the (nonexistent) executable. The earlier
# cascading "cannot find hello.exe" secondary failure - which obscured
# the real compiler error in the first real Windows run's log - cannot
# recur: the real failure is reported once, clearly, and nothing else is
# attempted for that example.
function Invoke-CuratedExample([string]$Name, [string]$ExpectedStdout, $Sanitized, [hashtable]$ExtraEnv = @{}) {
    $sourcePath = Join-Path $examplesDir $Name
    $baseName = [System.IO.Path]::GetFileNameWithoutExtension($Name)
    $outputPath = Join-Path $examplesDir $baseName
    $exePath = "$outputPath.exe"
    Remove-Item -LiteralPath $exePath -ErrorAction SilentlyContinue

    $compileResult = Invoke-Native -FilePath $kaiccExe -ArgumentList @($sourcePath, '-o', $outputPath) -WorkingDirectory $examplesDir -Sanitized $Sanitized -ExtraEnv $ExtraEnv
    $compileSucceeded = ($compileResult.ExitCode -eq 0) -and (Test-Path -LiteralPath $exePath -PathType Leaf)
    Assert-True ($compileResult.ExitCode -eq 0) "$Name compiles (exit 0) - stderr: $($compileResult.Stderr.Trim())"
    Assert-True (Test-Path -LiteralPath $exePath -PathType Leaf) "$baseName.exe was produced at the expected path"

    if (-not $compileSucceeded) {
        Write-Host "  SKIPPING run of $baseName.exe - compile did not succeed (fail-fast; see stderr above)"
        return
    }

    $runResult = Invoke-Native -FilePath $exePath -ArgumentList @() -WorkingDirectory $examplesDir -Sanitized $Sanitized
    Assert-True ($runResult.ExitCode -eq 0) "$baseName.exe exits 0"
    Assert-Equal $runResult.Stdout $ExpectedStdout "$baseName.exe produces the exact expected LF-only stdout"

    Remove-Item -LiteralPath $exePath -ErrorAction SilentlyContinue
}

Write-Section 'Default discovery (no KAI_CC set) - curated examples, exact output, path contains a space'
Invoke-CuratedExample -Name 'hello.kai' -ExpectedStdout "Hello from KAI`n" -Sanitized $sanitizedWithToolchain
Invoke-CuratedExample -Name 'functions.kai' -ExpectedStdout "Hello`nKAI`n42`n84`n" -Sanitized $sanitizedWithToolchain
Invoke-CuratedExample -Name 'conditions.kai' -ExpectedStdout "adult`npositive`nnegative`nzero`n" -Sanitized $sanitizedWithToolchain
Invoke-CuratedExample -Name 'variables.kai' -ExpectedStdout "KAI`n0.1`n2026`n1`n" -Sanitized $sanitizedWithToolchain

Write-Section 'KAI_CC explicit override (documented advanced path)'
Invoke-CuratedExample -Name 'hello.kai' -ExpectedStdout "Hello from KAI`n" -Sanitized $sanitizedWithToolchain -ExtraEnv @{ KAI_CC = $gccPath }

# ======================================================================
# 8. Repository/build-tree leakage checks (spec §20/§26, best-effort)
# ======================================================================
Write-Section 'Repository/build-tree leakage checks'
$suspiciousPatterns = @('build-release-windows', 'dist/kai-windows-x86_64', 'msys64', 'ucrt64')
$allCapturedText = @($versionResult.Stdout, $helpResult.Stdout, $missingLinkerResult.Stderr) -join "`n"
# Force array semantics with @(...): Where-Object returns $null (not an
# empty array) when nothing matches, and Set-StrictMode makes .Count on
# $null throw rather than silently evaluate to zero.
$leaked = @($suspiciousPatterns | Where-Object { $allCapturedText -match [regex]::Escape($_) })
Assert-True ($leaked.Count -eq 0) 'no captured kaicc.exe output references the KAI build tree or MSYS2 prefix'

# ======================================================================
# 9. VSIX install into an ISOLATED VS Code profile (spec §26/§27) -
#    optional: only attempted when both -VsixPath and -VSCodeZipPath were
#    given, so this script degrades gracefully rather than pretending.
# ======================================================================
if ($VsixPath -and $VSCodeZipPath) {
    $listResult = $null

    Write-Section 'VSIX structural validation (before invoking VS Code at all)'
    $vsixInfo = Test-VsixStructure -VsixPath $VsixPath
    Write-Host "  publisher: $($vsixInfo.Publisher)"
    Write-Host "  name:      $($vsixInfo.Name)"
    Write-Host "  version:   $($vsixInfo.Version)"
    Write-Host "  expected --list-extensions --show-versions entry: $($vsixInfo.ExpectedId)"
    if ($vsixInfo.TargetPlatform) {
        Write-Host "  vsixmanifest TargetPlatform: $($vsixInfo.TargetPlatform)"
        Assert-True ($vsixInfo.TargetPlatform -eq 'win32-x64') 'VSIX manifest declares targetPlatform win32-x64'
    } else {
        Write-Host '  vsixmanifest TargetPlatform: (not present in manifest - not all vsce versions encode it; not treated as a failure)'
    }

    Write-Section 'Installing the win32-x64 VSIX into an ISOLATED VS Code profile'
    $vscodeRoot = Join-Path $WorkDir 'VSCode'
    Expand-Archive -LiteralPath $VSCodeZipPath -DestinationPath $vscodeRoot -Force

    # VS CODE CLI ISOLATED-INSTALL DIAGNOSIS ROUND 2: a real Windows run
    # found that a loose recursive "cli.js" filename search
    # (Find-FileRecursive) can select the WRONG file - the pinned archive
    # ships THREE files literally named cli.js, and enumeration order
    # picked @vscode/sandbox-runtime's own internal cli.js instead of the
    # real one (it happily started, reported its own "1.0.0", then failed
    # --install-extension with "Sandbox dependencies not available").
    # Find-VSCodeCliJs (see its own doc comment) resolves ONLY the exact
    # structural shape code.cmd itself uses - <build-id>/resources/app/
    # out/cli.js - never a loose basename match, and fails closed on zero
    # or multiple candidates rather than "first match wins". Reproducing
    # code.cmd's real behavior directly (ELECTRON_RUN_AS_NODE=1 + Code.exe
    # + this resolved cli.js) still avoids cmd.exe entirely - the earlier
    # problem was never shell-free invocation itself, only which file it
    # pointed at.
    $codeExe = Join-Path $vscodeRoot 'Code.exe'
    $cliJsResolutionFailed = $false
    $cliJs = $null
    if (-not (Test-Path -LiteralPath $codeExe -PathType Leaf)) {
        Write-Host '  LIMITATION: Code.exe not found in the extracted VS Code archive - skipping VSIX install test.'
        $cliJsResolutionFailed = $true
    } else {
        try {
            $cliJs = Find-VSCodeCliJs -VSCodeRoot $vscodeRoot
            Write-Host "  resolved cli.js (structural match, <build-id>/resources/app/out/cli.js): $cliJs"
        } catch {
            Write-Host "  LIMITATION: $($_.Exception.Message) - skipping VSIX install test."
            $cliJsResolutionFailed = $true
        }

        # Optional cross-check (spec §4): code.cmd itself is the
        # authoritative wrapper shipped by this archive - confirm it at
        # least REFERENCES the same relative shape we resolved, as a
        # cheap sanity check. Deliberately not a general batch-file
        # parser - purely informational, never fatal on its own (the
        # structural resolver above and the version-identity check below
        # are the real authority).
        $codeCmdPath = Find-FileRecursive -Root $vscodeRoot -Name 'code.cmd'
        if ($codeCmdPath) {
            $codeCmdContent = Get-Content -LiteralPath $codeCmdPath -Raw
            if ($codeCmdContent -match [regex]::Escape('resources\app\out\cli.js')) {
                Write-Host '  code.cmd cross-check: references resources\app\out\cli.js, matching the resolved entrypoint'
            } else {
                Write-Host '  code.cmd cross-check: could not confirm the expected reference (informational only, not fatal)'
            }
        }
    }
    if ($cliJsResolutionFailed) {
        $script:FailureCount++
    } else {
        $userDataDir = Join-Path $WorkDir 'vscode-user-data'
        $extensionsDir = Join-Path $WorkDir 'vscode-extensions'
        $vscodeProfileRoot = Join-Path $WorkDir 'vscode-profile-env'
        New-Item -ItemType Directory -Path $userDataDir, $extensionsDir, $vscodeProfileRoot -Force | Out-Null
        $vscodeSanitized = New-VSCodeSanitizedEnvironment -IsolatedProfileRoot $vscodeProfileRoot

        # Preflight (spec §2/§5): prove the CLI itself starts AND is
        # genuinely the pinned VS Code build - not just "some cli.js
        # exited 0". This is the exact identity check that would have
        # caught the sandbox-runtime cli.js immediately (its own
        # "--version" reports "1.0.0", never the pinned VS Code version).
        $versionCheck = Invoke-Native -FilePath $codeExe -ArgumentList @($cliJs, '--version') -WorkingDirectory $vscodeRoot -Sanitized $vscodeSanitized
        Write-Host "  VS Code CLI --version: exit $($versionCheck.ExitCode)"
        Write-BoundedOutput 'stdout' $versionCheck.Stdout
        Write-BoundedOutput 'stderr' $versionCheck.Stderr
        Assert-True ($versionCheck.ExitCode -eq 0) 'VS Code CLI starts (Code.exe + resolved cli.js, --version)'

        $reportedVersion = if ($versionCheck.ExitCode -eq 0) { (($versionCheck.Stdout -split "`r?`n")[0]).Trim() } else { $null }
        $identityConfirmed = ($versionCheck.ExitCode -eq 0)
        if ($identityConfirmed -and $ExpectedVSCodeVersion) {
            Write-Host "  expected VS Code version (pinned): $ExpectedVSCodeVersion"
            Write-Host "  actual reported version:           $reportedVersion"
            $identityConfirmed = ($reportedVersion -ceq $ExpectedVSCodeVersion)
            Assert-True $identityConfirmed "resolved cli.js reports the pinned VS Code version '$ExpectedVSCodeVersion' (this is the exact identity check that catches a wrong-cli.js regression - resolved path: $cliJs)"
        } elseif ($identityConfirmed) {
            Assert-True ($reportedVersion.Length -gt 0) 'VS Code CLI --version produced a non-empty version line (no -ExpectedVSCodeVersion given, so identity was not strictly verified)'
        }

        if (-not $identityConfirmed) {
            Write-Host '  SKIPPING install/list - the resolved VS Code CLI did not start, or its identity did not match the pinned version (fail closed rather than trusting the wrong cli.js).'
        } else {
            $installArgs = @($cliJs, '--user-data-dir', $userDataDir, '--extensions-dir', $extensionsDir, '--install-extension', $VsixPath, '--force')
            $installResult = Invoke-Native -FilePath $codeExe -ArgumentList $installArgs -WorkingDirectory $vscodeRoot -Sanitized $vscodeSanitized
            Write-Host "  --install-extension: exit $($installResult.ExitCode)"
            Write-BoundedOutput 'stdout' $installResult.Stdout
            Write-BoundedOutput 'stderr' $installResult.Stderr
            Assert-True ($installResult.ExitCode -eq 0) "VSIX installed into an isolated --user-data-dir/--extensions-dir (exit $($installResult.ExitCode))"

            if ($installResult.ExitCode -ne 0) {
                Write-Host '  SKIPPING --list-extensions - install did not succeed (fail-fast; see stdout/stderr above)'
            } else {
                $listArgs = @($cliJs, '--user-data-dir', $userDataDir, '--extensions-dir', $extensionsDir, '--list-extensions', '--show-versions')
                $listResult = Invoke-Native -FilePath $codeExe -ArgumentList $listArgs -WorkingDirectory $vscodeRoot -Sanitized $vscodeSanitized
                Write-Host "  --list-extensions --show-versions: exit $($listResult.ExitCode)"
                Write-BoundedOutput 'stdout' $listResult.Stdout
                Write-BoundedOutput 'stderr' $listResult.Stderr
                Assert-True ($listResult.Stdout -match [regex]::Escape($vsixInfo.ExpectedId)) "installed extension appears as exactly '$($vsixInfo.ExpectedId)' in --list-extensions --show-versions"
            }
        }
    }
} else {
    Write-Section 'VSIX install test: SKIPPED (documented limitation)'
    Write-Host '  -VsixPath and/or -VSCodeZipPath were not supplied - the real isolated VSIX'
    Write-Host '  install test did not run this invocation. The strongest remaining evidence'
    Write-Host '  is VS CODE WINDOWS M4''s own real extension-smoke (bundled kaicc.exe resolved,'
    Write-Host '  hello.kai compiled and run) plus this script''s own compiler-level proof above.'
}

# ======================================================================
# Summary (spec §42, extended by CI PORTABILITY FIX spec §11) - concise,
# no marketing claims. Explicitly distinguishes the KAI fresh-user root
# (spaces supported) from the host toolchain prefix (must be space-free)
# rather than a single blended "paths with spaces" claim.
# ======================================================================
Write-Host ''
if ($script:FailureCount -eq 0) {
    Write-Host 'KAI Windows fresh-user smoke: PASS'
} else {
    Write-Host "KAI Windows fresh-user smoke: FAIL ($script:FailureCount check(s) failed)"
}
Write-Host ''
Write-Host "compiler startup without dev environment: $(if ($versionResult.ExitCode -eq 0 -and $helpResult.ExitCode -eq 0) { 'PASS' } else { 'FAIL' })"
Write-Host "semantic tooling without linker: $(if ($inspectResult.ExitCode -eq 0 -and $referencesResult.ExitCode -eq 0 -and $callGraphResult.ExitCode -eq 0) { 'PASS' } else { 'FAIL' })"
Write-Host "missing-linker diagnostic: $(if ($missingLinkerResult.ExitCode -eq 9) { 'PASS' } else { 'FAIL' })"
Write-Host "standalone host linker discovery: $(if ($gccVersionResult.ExitCode -eq 0) { 'PASS' } else { 'FAIL' })"
Write-Host "host toolchain preflight (space-free prefix): $(if ($freeResult.Success) { 'PASS' } else { 'FAIL' })"
Write-Host "curated native examples: $(if ($script:FailureCount -eq 0) { 'PASS' } else { 'SEE ABOVE' })"
if ($VsixPath -and $VSCodeZipPath) {
    Write-Host "VSIX installation: $(if ($listResult -and $vsixInfo -and $listResult.Stdout -match [regex]::Escape($vsixInfo.ExpectedId)) { 'PASS' } else { 'FAIL' })"
} else {
    Write-Host 'VSIX installation: documented limitation (not attempted this invocation)'
}
Write-Host "KAI fresh-user root contains spaces: $(if ($WorkDir -match ' ') { 'PASS' } else { 'FAIL' })"
Write-Host "host toolchain prefix contains spaces: $(if ($toolchainWorkDir -match '\s') { 'YES (unexpected)' } else { 'NO' })"
Write-Host ''
Write-Host "KAI Windows ZIP size: $kaiZipSizeMb MB"
if ($VsixPath) { Write-Host "KAI Windows VSIX size: $vsixSizeMb MB" }

if ($script:FailureCount -gt 0) {
    exit 1
}
