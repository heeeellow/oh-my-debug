[CmdletBinding()]
param([Parameter(Mandatory = $true)][string]$BundleDirectory)

$ErrorActionPreference = 'Stop'
$bundleRoot = [System.IO.Path]::GetFullPath($BundleDirectory)
$manifestPath = Join-Path $bundleRoot 'manifest.json'
if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) { throw "Agent Bundle manifest not found: $manifestPath" }
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
if ($manifest.schema_version -ne 2) { throw 'Unsupported Agent Bundle manifest schema; expected v2.' }
if ([string]$manifest.product -ne 'TestTools') { throw 'Manifest product is not TestTools.' }
$versionHeader = Join-Path (Split-Path -Parent $PSScriptRoot) 'src\core\protocol\agent_version.h'
$versionText = Get-Content -LiteralPath $versionHeader -Raw
$expectedAgentVersion = [regex]::Match($versionText, 'AgentVersion\[\]\s*=\s*"(?<version>[^"]+)"').Groups['version'].Value
if ([string]::IsNullOrWhiteSpace($expectedAgentVersion) -or
    [string]$manifest.agent_version -ne $expectedAgentVersion) {
    throw "Manifest agent_version does not match current source: expected $expectedAgentVersion, got $($manifest.agent_version)."
}
if ($manifest.protocol.minimum -ne 1 -or $manifest.protocol.maximum -ne 1) { throw 'Manifest protocol range is not v1.' }
if ($manifest.packages.Count -ne 3) { throw 'Manifest must contain exactly three packages.' }

$expected = @{
    'android/armeabi-v7a' = @{ platform='android'; architecture='armv7'; root='agents/android/armeabi-v7a'; abi='armeabi-v7a' }
    'android/arm64-v8a' = @{ platform='android'; architecture='aarch64'; root='agents/android/arm64-v8a'; abi='arm64-v8a' }
    'linux/aarch64/glibc-2.31' = @{ platform='linux'; architecture='aarch64'; root='agents/linux/aarch64/glibc-2.31'; libc='glibc'; minimum_version='2.31' }
}
$seenIds = [System.Collections.Generic.HashSet[string]]::new()
$seenPaths = [System.Collections.Generic.HashSet[string]]::new()
foreach ($package in $manifest.packages) {
    $id = [string]$package.id
    if (-not $seenIds.Add($id) -or -not $expected.ContainsKey($id)) { throw "Unexpected or duplicate package id: $id" }
    $rule = $expected[$id]
    if ([string]$package.platform -ne $rule.platform -or [string]$package.architecture -ne $rule.architecture -or [string]$package.root -ne $rule.root) { throw "Package identity or root mismatch: $id" }
    if ([string]$package.root -notmatch '^[A-Za-z0-9._/-]+$' -or [string]$package.root -match '(^|[\/])\.\.([\/]|$)' -or [System.IO.Path]::IsPathRooted([string]$package.root)) { throw "Unsafe package root: $id" }
    if ($rule.abi) {
        if ([string]$package.android.abi -ne $rule.abi -or [int]$package.android.minimum_api -ne 23) { throw "Android package metadata mismatch: $id" }
    } elseif ([string]$package.linux.libc -ne 'glibc' -or [string]$package.linux.minimum_version -ne '2.31') { throw "Linux package metadata mismatch: $id" }
    if ($package.files.Count -lt 1 -or $package.files.Count -gt 128) {
        throw "Package file count is invalid: $id"
    }
    $agentCount = 0
    foreach ($file in $package.files) {
        $relative = [string]$file.path
        if ([string]::IsNullOrWhiteSpace($relative) -or $relative -notmatch '^[A-Za-z0-9._/-]+$' -or [System.IO.Path]::IsPathRooted($relative) -or $relative -match '(^|[\/])\.\.([\/]|$)' -or $relative.Contains('\') -or $relative.Contains(':')) { throw "Unsafe install path: $id/$relative" }
        $canonical = "$($package.root)/$relative"
        if (-not $seenPaths.Add($canonical)) { throw "Duplicate install path: $canonical" }
        $filePath = Join-Path $bundleRoot ($canonical -replace '/', '\')
        if (-not (Test-Path -LiteralPath $filePath -PathType Leaf)) { throw "Missing bundle file: $canonical" }
        $actual = Get-Item -LiteralPath $filePath
        if ([int64]$file.size -ne $actual.Length) { throw "Size mismatch: $canonical" }
        $hash = (Get-FileHash -LiteralPath $filePath -Algorithm SHA256).Hash.ToLowerInvariant()
        if ([string]$file.sha256 -ne $hash -or [string]$file.sha256 -notmatch '^[0-9a-fA-F]{64}$') { throw "SHA-256 mismatch: $canonical" }
        if ([string]::IsNullOrWhiteSpace([string]$file.role) -or $null -eq $file.executable) { throw "File role or executable flag missing: $canonical" }
        if ([string]$file.role -eq 'agent') { ++$agentCount }
    }
    $agent = @($package.files | Where-Object { [string]$_.role -eq 'agent' })
    if ($agentCount -ne 1 -or [string]$agent[0].path -ne 'testtools-agent' -or
        -not [bool]$agent[0].executable) {
        throw "Package must contain one executable testtools-agent: $id"
    }
}
if ($seenIds.Count -ne 3) { throw 'Manifest is missing one or more required packages.' }
Write-Host ("Agent Bundle v2 valid: {0} packages; all files, sizes and SHA-256 values match." -f $manifest.packages.Count)
