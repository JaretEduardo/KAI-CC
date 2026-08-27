<#
.SYNOPSIS
    WINDOWS FRESH-USER M5.1: verifies a downloaded file's SHA-256 against
    the pinned digest recorded in scripts/windows-fresh-user-pins.json
    BEFORE anything else is allowed to extract or execute it. A versioned
    URL alone is not an immutable-byte guarantee - this is the actual
    supply-chain check for the two external, third-party archives
    windows-fresh-user CI downloads (the standalone host toolchain and
    the portable VS Code archive).

    Deliberately tiny and dependency-free: Get-FileHash is a built-in
    PowerShell cmdlet, no external hashing tool is required. Runnable
    identically in CI or by a maintainer verifying a download by hand.

.PARAMETER Path
    Path to the already-downloaded file to verify.

.PARAMETER Component
    Which entry in the pins file to check against ("winlibs" or "vscode").

.PARAMETER PinsFile
    Path to the pins JSON (defaults to windows-fresh-user-pins.json next
    to this script) - the single source of truth for URL/version/SHA-256,
    never duplicated elsewhere.

.EXAMPLE
    ./scripts/verify-pinned-download.ps1 -Path fresh-user-input/toolchain.zip -Component winlibs
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Path,
    [Parameter(Mandatory = $true)][string]$Component,
    [string]$PinsFile = (Join-Path $PSScriptRoot 'windows-fresh-user-pins.json')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
    throw "verify-pinned-download: file not found: $Path"
}
if (-not (Test-Path -LiteralPath $PinsFile -PathType Leaf)) {
    throw "verify-pinned-download: pins file not found: $PinsFile"
}

$pins = Get-Content -LiteralPath $PinsFile -Raw | ConvertFrom-Json
if (-not ($pins.PSObject.Properties.Name -contains $Component)) {
    throw "verify-pinned-download: no pinned entry for component '$Component' in $PinsFile"
}
$expected = $pins.$Component.sha256
if ([string]::IsNullOrWhiteSpace($expected)) {
    throw "verify-pinned-download: pinned entry for '$Component' in $PinsFile has no sha256 value"
}

$actual = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash

if ($actual.ToUpperInvariant() -ne $expected.ToUpperInvariant()) {
    Write-Host "SHA-256 MISMATCH for '$Component' - refusing to extract or execute this archive."
    Write-Host "  file:     $Path"
    Write-Host "  expected: $expected"
    Write-Host "  actual:   $actual"
    throw "verify-pinned-download: SHA-256 mismatch for '$Component'."
}

Write-Host "verify-pinned-download: '$Component' SHA-256 verified ($actual)"
