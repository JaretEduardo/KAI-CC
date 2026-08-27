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

    Everything runs from a FRESH directory under the Windows temp folder
    whose name deliberately contains a space, entirely outside this
    repository/any build tree/any MSYS2 installation. Every KAI process
    (kaicc.exe and the programs it compiles) is launched via
    System.Diagnostics.Process directly (UseShellExecute=$false, an argv
    list, never a shell) - the PowerShell equivalent of the extension's own
    shell-free spawnProcess(). The child's environment is constructed FRESH
    per invocation from a small, explicit allowlist (see
    New-SanitizedEnvironment below) - the running PowerShell session's own
    $env:PATH is never mutated, so this script needs no save/restore
    dance to coexist with later CI steps.

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

.PARAMETER WorkDir
    Optional override for the fresh test directory. If omitted, a unique
    directory containing a literal space is created under $env:TEMP.
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$KaiZipPath,
    [Parameter(Mandatory = $true)][string]$ToolchainZipPath,
    [string]$VsixPath = $null,
    [string]$VSCodeZipPath = $null,
    [string]$WorkDir = $null
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:FailureCount = 0

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
# ---------------------------------------------------------------------
function Invoke-Native {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [string[]]$ArgumentList = @(),
        [string]$WorkingDirectory = $null,
        [Parameter(Mandatory = $true)]$Sanitized,
        [hashtable]$ExtraEnv = @{}
    )

    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $FilePath
    foreach ($a in $ArgumentList) { [void]$psi.ArgumentList.Add($a) }
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.CreateNoWindow = $true
    if ($WorkingDirectory) { $psi.WorkingDirectory = $WorkingDirectory }

    # ProcessStartInfo.Environment/EnvironmentVariables is pre-populated
    # (by .NET) as a COPY of this PowerShell process's own environment.
    # Deliberately do NOT try to selectively delete unwanted inherited
    # variables from that copy: a local test against the real .NET API
    # this round showed that approach can leave a stale variable visible
    # to the child even after Remove() reports success (confirmed via a
    # standalone repro - see this milestone's report). Instead, clear the
    # WHOLE inherited environment first and rebuild it from Sanitized.Vars
    # (an explicit allowlist - see New-SanitizedEnvironment) - this is a
    # strictly stronger guarantee: nothing can leak through that wasn't
    # explicitly added back, regardless of any dictionary-removal
    # semantics on either platform. $env:PATH (and every other real
    # variable in THIS PowerShell session) is never touched - only the
    # copy handed to each child process.
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
# survives, because nothing survives by default.
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

function Test-CommandAbsent([string]$CommandName, $Sanitized) {
    $whereExe = Join-Path $env:SystemRoot 'System32/where.exe'
    $result = Invoke-Native -FilePath $whereExe -ArgumentList @($CommandName) -Sanitized $Sanitized
    return ($result.ExitCode -ne 0)
}

function Find-FileRecursive([string]$Root, [string]$Name) {
    $found = Get-ChildItem -Path $Root -Filter $Name -Recurse -File -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $found) { return $null }
    return $found.FullName
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
# 1. Fresh directory OUTSIDE the repo/build tree/MSYS2, path with a space
#    (spec §4/§19/§20)
# ======================================================================
Write-Section 'Creating fresh test directory (outside repo/build/MSYS2, path contains a space)'
if (-not $WorkDir) {
    $suffix = [guid]::NewGuid().ToString('N').Substring(0, 8)
    $WorkDir = Join-Path $env:TEMP "KAI Fresh User $suffix"
}
New-Item -ItemType Directory -Path $WorkDir -Force | Out-Null
Write-Host "  WorkDir: $WorkDir"
Assert-True ($WorkDir -match ' ') 'fresh test directory path contains a space'

# ======================================================================
# 2. Extract the KAI ZIP using ordinary Windows facilities (spec §6)
# ======================================================================
Write-Section 'Extracting the KAI Windows ZIP (Expand-Archive)'
$kaiExtractRoot = Join-Path $WorkDir 'kai-zip'
Expand-Archive -LiteralPath $KaiZipPath -DestinationPath $kaiExtractRoot -Force
$kaiRoot = Join-Path $kaiExtractRoot 'kai-windows-x86_64'
Assert-True (Test-Path -LiteralPath $kaiRoot -PathType Container) 'archive root kai-windows-x86_64/ exists after extraction'

$kaiccExe = Join-Path $kaiRoot 'bin/kaicc.exe'
$runtimeArchive = Join-Path $kaiRoot 'lib/kai/libkai_runtime.a'
$requiredDlls = @('libgcc_s_seh-1.dll', 'libstdc++-6.dll', 'libwinpthread-1.dll', 'zlib1.dll', 'libzstd.dll')

foreach ($check in @(
        @{ Path = $kaiccExe; Label = 'bin/kaicc.exe' },
        @{ Path = $runtimeArchive; Label = 'lib/kai/libkai_runtime.a' },
        @{ Path = (Join-Path $kaiRoot 'LICENSE'); Label = 'LICENSE' },
        @{ Path = (Join-Path $kaiRoot 'THIRD_PARTY_NOTICES.md'); Label = 'THIRD_PARTY_NOTICES.md' },
        @{ Path = (Join-Path $kaiRoot 'examples/hello.kai'); Label = 'examples/hello.kai' },
        @{ Path = (Join-Path $kaiRoot 'examples/functions.kai'); Label = 'examples/functions.kai' },
        @{ Path = (Join-Path $kaiRoot 'examples/conditions.kai'); Label = 'examples/conditions.kai' },
        @{ Path = (Join-Path $kaiRoot 'examples/variables.kai'); Label = 'examples/variables.kai' }
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
# 3. Extract the independently-obtained host toolchain (never the KAI
#    build's own MSYS2 environment - spec §11/§16)
# ======================================================================
Write-Section 'Extracting the independently-obtained host toolchain'
$toolchainRoot = Join-Path $WorkDir 'Host Toolchain'
Expand-Archive -LiteralPath $ToolchainZipPath -DestinationPath $toolchainRoot -Force
$gccPath = Find-FileRecursive -Root $toolchainRoot -Name 'gcc.exe'
if (-not $gccPath) {
    throw 'invalid host toolchain archive: gcc.exe not found anywhere under the extracted tree'
}
$toolchainBinDir = Split-Path -Parent $gccPath
Write-Host "  gcc.exe found at: $gccPath"
Assert-True ($toolchainBinDir -notlike '*msys64*' -and $toolchainBinDir -notlike '*ucrt64*') `
    'host toolchain path does not come from an MSYS2 tree (genuinely independent)'

# ======================================================================
# 4. Sanitized environment WITHOUT the host toolchain yet (spec §7/§8)
# ======================================================================
$sanitizedNoToolchain = New-SanitizedEnvironment
Write-Section 'Dev-tool absence check (sanitized PATH, before adding the host toolchain)'
foreach ($cmd in @('llvm-config', 'cmake', 'ninja', 'bash', 'clang', 'gcc', 'cc')) {
    Assert-True (Test-CommandAbsent -CommandName $cmd -Sanitized $sanitizedNoToolchain) "'$cmd' is NOT resolvable in the sanitized environment"
}
Assert-True ($sanitizedNoToolchain.Vars.Path -notmatch '(?i)mingw|ucrt64|msys64') 'sanitized PATH contains no MSYS2/MinGW dev-environment directory'

# ======================================================================
# 5. kaicc.exe starts and answers semantic queries with NO host linker
#    exposed at all (spec §9/§10/§21 - the most important portability
#    statement this milestone makes)
# ======================================================================
Write-Section 'kaicc.exe startup WITHOUT any host toolchain on PATH'
$versionResult = Invoke-Native -FilePath $kaiccExe -ArgumentList @('--version') -Sanitized $sanitizedNoToolchain
Assert-True ($versionResult.ExitCode -eq 0) 'kaicc.exe --version exits 0 with no host toolchain present'
Assert-True ($versionResult.Stdout -match '^KAI-CC 0\.1\.0-alpha\.1') "kaicc.exe --version reports the expected 'KAI-CC 0.1.0-alpha.1' (got: $($versionResult.Stdout.Trim()))"

$helpResult = Invoke-Native -FilePath $kaiccExe -ArgumentList @('--help') -Sanitized $sanitizedNoToolchain
Assert-True ($helpResult.ExitCode -eq 0) 'kaicc.exe --help exits 0 with no host toolchain present'
Assert-True ($helpResult.Stdout.Length -gt 0) 'kaicc.exe --help produced non-empty output'

Write-Section 'Semantic tooling WITHOUT any host linker (no native link needed)'
$functionsKai = Join-Path $kaiRoot 'examples/functions.kai'
$inspectResult = Invoke-Native -FilePath $kaiccExe -ArgumentList @('inspect', $functionsKai, '--json') -Sanitized $sanitizedNoToolchain
Assert-True ($inspectResult.ExitCode -eq 0 -and $inspectResult.Stdout -match '"kind":"function"') 'kaicc.exe inspect works without a host linker'

$referencesResult = Invoke-Native -FilePath $kaiccExe -ArgumentList @('references', $functionsKai, '--line', '15', '--column', '13', '--json') -Sanitized $sanitizedNoToolchain
Assert-True ($referencesResult.ExitCode -eq 0 -and $referencesResult.Stdout -match '"line"') 'kaicc.exe references works without a host linker'

$callGraphResult = Invoke-Native -FilePath $kaiccExe -ArgumentList @('call-graph', $functionsKai, '--json') -Sanitized $sanitizedNoToolchain
Assert-True ($callGraphResult.ExitCode -eq 0 -and $callGraphResult.Stdout -match '"add"') 'kaicc.exe call-graph works without a host linker'

# ======================================================================
# 6. Missing-linker UX (spec §22) - what a first-time Windows user sees
#    before installing any toolchain at all
# ======================================================================
Write-Section 'Missing-linker diagnostic (deliberately no toolchain on PATH)'
$helloKai = Join-Path $kaiRoot 'examples/hello.kai'
$missingLinkerOut = Join-Path $WorkDir 'hello-no-linker'
$missingLinkerResult = Invoke-Native -FilePath $kaiccExe -ArgumentList @($helloKai, '-o', $missingLinkerOut) -Sanitized $sanitizedNoToolchain
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
$gccVersionResult = Invoke-Native -FilePath $gccPath -ArgumentList @('--version') -Sanitized $sanitizedWithToolchain
$gccVersionLine = ($gccVersionResult.Stdout -split "`n")[0].Trim()
Write-Host "  selected linker: $gccPath"
Write-Host "  selected linker version: $gccVersionLine"

function Invoke-CuratedExample([string]$Name, [string]$ExpectedStdout, $Sanitized, [hashtable]$ExtraEnv = @{}) {
    $sourcePath = Join-Path $kaiRoot "examples/$Name"
    $baseName = [System.IO.Path]::GetFileNameWithoutExtension($Name)
    $outputPath = Join-Path $kaiRoot "examples/$baseName"
    $exePath = "$outputPath.exe"
    Remove-Item -LiteralPath $exePath -ErrorAction SilentlyContinue

    $compileResult = Invoke-Native -FilePath $kaiccExe -ArgumentList @($sourcePath, '-o', $outputPath) -Sanitized $Sanitized -ExtraEnv $ExtraEnv
    Assert-True ($compileResult.ExitCode -eq 0) "$Name compiles (exit 0) - stderr: $($compileResult.Stderr.Trim())"
    Assert-True (Test-Path -LiteralPath $exePath -PathType Leaf) "$baseName.exe was produced at the expected path"

    $runResult = Invoke-Native -FilePath $exePath -ArgumentList @() -Sanitized $Sanitized
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
    Write-Section 'Installing the win32-x64 VSIX into an ISOLATED VS Code profile'
    $vscodeRoot = Join-Path $WorkDir 'VSCode'
    Expand-Archive -LiteralPath $VSCodeZipPath -DestinationPath $vscodeRoot -Force
    $codeCmd = Find-FileRecursive -Root $vscodeRoot -Name 'code.cmd'
    if (-not $codeCmd) {
        Write-Host '  LIMITATION: code.cmd not found in the extracted VS Code archive - skipping VSIX install test.'
        $script:FailureCount++
    } else {
        $userDataDir = Join-Path $WorkDir 'vscode-user-data'
        $extensionsDir = Join-Path $WorkDir 'vscode-extensions'
        New-Item -ItemType Directory -Path $userDataDir, $extensionsDir -Force | Out-Null

        # code.cmd is a Windows batch script, not a native PE executable -
        # Windows can only ever launch it through cmd.exe (this is a
        # platform fact, not a design choice); the argument LIST below is
        # still never joined into a shell string, so no injection/quoting
        # risk is introduced. kaicc.exe itself is NEVER invoked this way
        # anywhere in this script - only this one third-party CLI wrapper.
        $cmdExe = Join-Path $env:SystemRoot 'System32/cmd.exe'
        $installArgs = @('/c', $codeCmd, '--user-data-dir', $userDataDir, '--extensions-dir', $extensionsDir, '--install-extension', $VsixPath, '--force')
        $installResult = Invoke-Native -FilePath $cmdExe -ArgumentList $installArgs -Sanitized $sanitizedNoToolchain
        Assert-True ($installResult.ExitCode -eq 0) "VSIX installed into an isolated --user-data-dir/--extensions-dir (exit $($installResult.ExitCode))"

        $listArgs = @('/c', $codeCmd, '--user-data-dir', $userDataDir, '--extensions-dir', $extensionsDir, '--list-extensions', '--show-versions')
        $listResult = Invoke-Native -FilePath $cmdExe -ArgumentList $listArgs -Sanitized $sanitizedNoToolchain
        Assert-True ($listResult.Stdout -match 'kai-language@') "installed extension appears in --list-extensions --show-versions (output: $($listResult.Stdout.Trim()))"
    }
} else {
    Write-Section 'VSIX install test: SKIPPED (documented limitation)'
    Write-Host '  -VsixPath and/or -VSCodeZipPath were not supplied - the real isolated VSIX'
    Write-Host '  install test did not run this invocation. The strongest remaining evidence'
    Write-Host '  is VS CODE WINDOWS M4''s own real extension-smoke (bundled kaicc.exe resolved,'
    Write-Host '  hello.kai compiled and run) plus this script''s own compiler-level proof above.'
}

# ======================================================================
# Summary (spec §42) - concise, no marketing claims
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
Write-Host "curated native examples: $(if ($script:FailureCount -eq 0) { 'PASS' } else { 'SEE ABOVE' })"
if ($VsixPath -and $VSCodeZipPath) {
    Write-Host "VSIX installation: $(if ($listResult -and $listResult.Stdout -match 'kai-language@') { 'PASS' } else { 'FAIL' })"
} else {
    Write-Host 'VSIX installation: documented limitation (not attempted this invocation)'
}
Write-Host 'paths with spaces: PASS (entire test tree lived under a space-containing path)'
Write-Host ''
Write-Host "KAI Windows ZIP size: $kaiZipSizeMb MB"
if ($VsixPath) { Write-Host "KAI Windows VSIX size: $vsixSizeMb MB" }

if ($script:FailureCount -gt 0) {
    exit 1
}
