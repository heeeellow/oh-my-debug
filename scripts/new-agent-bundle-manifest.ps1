[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$BundleDirectory,
    [Parameter(Mandatory = $true)]
    [string]$BuildId,
    [Parameter(Mandatory = $true)]
    [string]$BuiltAtUtc,
    [ValidateSet('Release')]
    [string]$Configuration = 'Release',
    [Parameter(Mandatory = $true)]
    [string]$AndroidNdkVersion,
    [Parameter(Mandatory = $true)]
    [string]$LinuxToolchainId
)

$ErrorActionPreference = 'Stop'
$bundleRoot = [System.IO.Path]::GetFullPath($BundleDirectory)
$versionHeader = Join-Path (Split-Path -Parent $PSScriptRoot) 'src\core\protocol\agent_version.h'
$versionText = Get-Content -LiteralPath $versionHeader -Raw
$agentVersion = [regex]::Match($versionText, 'AgentVersion\[\]\s*=\s*"(?<version>[^"]+)"').Groups['version'].Value
$protocolVersion = [regex]::Match($versionText, 'AgentProtocolVersion\s*=\s*(?<version>\d+)').Groups['version'].Value
if ([string]::IsNullOrWhiteSpace($agentVersion) -or [string]::IsNullOrWhiteSpace($protocolVersion)) {
    throw 'Unable to read Agent version and protocol from agent_version.h.'
}

function New-Artifact {
    param(
        [string]$RelativePath,
        [string]$Platform,
        [string]$Architecture,
        [hashtable]$SpecificMetadata
    )
    $fullPath = Join-Path $bundleRoot ($RelativePath -replace '/', '\')
    if (-not (Test-Path -LiteralPath $fullPath -PathType Leaf)) { throw "Missing Agent artifact: $RelativePath" }
    $file = Get-Item -LiteralPath $fullPath
    $artifact = [ordered]@{
        platform = $Platform
        architecture = $Architecture
        path = $RelativePath
        size = [int64]$file.Length
        sha256 = (Get-FileHash -LiteralPath $fullPath -Algorithm SHA256).Hash.ToLowerInvariant()
        build = [ordered]@{ configuration = $Configuration; build_id = $BuildId }
    }
    foreach ($key in $SpecificMetadata.Keys) { $artifact[$key] = $SpecificMetadata[$key] }
    return $artifact
}

$artifacts = @(
    (New-Artifact 'agents/android/armeabi-v7a/testtools-agent' 'android' 'armv7' ([ordered]@{
        android = [ordered]@{ abi = 'armeabi-v7a'; minimum_api = 23; stl = 'c++_static'; ndk = $AndroidNdkVersion }
    }))
    (New-Artifact 'agents/android/arm64-v8a/testtools-agent' 'android' 'aarch64' ([ordered]@{
        android = [ordered]@{ abi = 'arm64-v8a'; minimum_api = 23; stl = 'c++_static'; ndk = $AndroidNdkVersion }
    }))
    (New-Artifact 'agents/linux/aarch64/glibc-2.31/testtools-agent' 'linux' 'aarch64' ([ordered]@{
        linux = [ordered]@{ libc = 'glibc'; minimum_version = '2.31'; toolchain = $LinuxToolchainId }
    }))
)

$manifest = [ordered]@{
    schema_version = 1
    product = 'TestTools'
    agent_version = $agentVersion
    protocol = [ordered]@{ minimum = [int]$protocolVersion; maximum = [int]$protocolVersion }
    build = [ordered]@{
        configuration = $Configuration
        build_id = $BuildId
        built_at_utc = $BuiltAtUtc
    }
    artifacts = $artifacts
}

$json = $manifest | ConvertTo-Json -Depth 8
[System.IO.File]::WriteAllText((Join-Path $bundleRoot 'manifest.json'), $json + [Environment]::NewLine, [System.Text.UTF8Encoding]::new($false))
Write-Host "Agent Bundle manifest written: $(Join-Path $bundleRoot 'manifest.json')"
