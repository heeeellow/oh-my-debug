[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$BundleDirectory
)

$ErrorActionPreference = 'Stop'
$bundleRoot = [System.IO.Path]::GetFullPath($BundleDirectory)
$manifestPath = Join-Path $bundleRoot 'manifest.json'
if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
    throw "Agent Bundle manifest not found: $manifestPath"
}

$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
if ($manifest.schema_version -ne 1) { throw 'Unsupported Agent Bundle manifest schema.' }
if ([string]::IsNullOrWhiteSpace([string]$manifest.agent_version)) { throw 'Manifest agent_version is empty.' }
if ($manifest.protocol.minimum -ne 1 -or $manifest.protocol.maximum -ne 1) {
    throw 'Manifest protocol range is not v1.'
}
if ($manifest.artifacts.Count -ne 3) { throw 'Manifest must contain exactly three Agent artifacts.' }

$expected = @{
    'agents/android/armeabi-v7a/testtools-agent' = @{ platform = 'android'; architecture = 'armv7'; abi = 'armeabi-v7a'; libc = $null }
    'agents/android/arm64-v8a/testtools-agent'   = @{ platform = 'android'; architecture = 'aarch64'; abi = 'arm64-v8a'; libc = $null }
    'agents/linux/aarch64/glibc-2.31/testtools-agent' = @{ platform = 'linux'; architecture = 'aarch64'; abi = $null; libc = 'glibc-2.31' }
}

$seen = [System.Collections.Generic.HashSet[string]]::new()
foreach ($artifact in $manifest.artifacts) {
    $relativePath = [string]$artifact.path
    if ([string]::IsNullOrWhiteSpace($relativePath) -or
        [System.IO.Path]::IsPathRooted($relativePath) -or
        $relativePath.Contains('..') -or
        $relativePath -notmatch '^[^\\/]+/[^\\/]+/') {
        throw "Manifest path is not a safe relative path: $relativePath"
    }
    if (-not $seen.Add($relativePath)) { throw "Duplicate manifest path: $relativePath" }
    if (-not $expected.ContainsKey($relativePath)) { throw "Unexpected manifest path: $relativePath" }

    $filePath = Join-Path $bundleRoot ($relativePath -replace '/', '\')
    if (-not (Test-Path -LiteralPath $filePath -PathType Leaf)) { throw "Missing bundle file: $relativePath" }
    $file = Get-Item -LiteralPath $filePath
    $hash = (Get-FileHash -LiteralPath $filePath -Algorithm SHA256).Hash.ToLowerInvariant()
    if ([int64]$artifact.size -ne $file.Length) { throw "Size mismatch: $relativePath" }
    if ([string]$artifact.sha256 -ne $hash) { throw "SHA-256 mismatch: $relativePath" }

    $rule = $expected[$relativePath]
    if ([string]$artifact.platform -ne $rule.platform -or
        [string]$artifact.architecture -ne $rule.architecture) {
        throw "Platform or architecture mismatch: $relativePath"
    }
    if ($rule.abi) {
        if ([string]$artifact.android.abi -ne $rule.abi -or [int]$artifact.android.minimum_api -ne 23) {
            throw "Android ABI or minimum API mismatch: $relativePath"
        }
    } else {
        if ([string]$artifact.linux.libc -ne 'glibc' -or [string]$artifact.linux.minimum_version -ne '2.31') {
            throw "Linux libc baseline mismatch: $relativePath"
        }
    }
}

foreach ($requiredPath in $expected.Keys) {
    if (-not $seen.Contains($requiredPath)) { throw "Manifest is missing: $requiredPath" }
}

Write-Host ("Agent Bundle valid: {0} artifacts; all sizes and SHA-256 values match." -f $manifest.artifacts.Count)
