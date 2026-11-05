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

function New-Package {
    param(
        [string]$RelativePath,
        [string]$Platform,
        [string]$Architecture,
        [hashtable]$SpecificMetadata
    )
    $fullPath = Join-Path $bundleRoot ($RelativePath -replace '/', '\')
    if (-not (Test-Path -LiteralPath $fullPath -PathType Leaf)) { throw "Missing Agent artifact: $RelativePath" }
    $file = Get-Item -LiteralPath $fullPath
    $separator = $RelativePath.LastIndexOf('/')
    $root = $RelativePath.Substring(0, $separator)
    $entryPath = $RelativePath.Substring($separator + 1)
    $fileEntry = [ordered]@{
        path = $entryPath
        role = 'agent'
        executable = $true
        size = [int64]$file.Length
        sha256 = (Get-FileHash -LiteralPath $fullPath -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    $package = [ordered]@{
        id = "$Platform/$($SpecificMetadata.id)"
        platform = $Platform
        architecture = $Architecture
        root = $root
        files = @($fileEntry)
    }
    $runnerPath = Join-Path $bundleRoot (($root + '/iperf3') -replace '/', '\')
    if (-not (Test-Path -LiteralPath $runnerPath -PathType Leaf)) { throw "Missing iperf3 Runner: $runnerPath" }
    $runner = Get-Item -LiteralPath $runnerPath
    $package.files += [ordered]@{
        path = 'iperf3'; role = 'runner'; executable = $true
        size = [int64]$runner.Length
        sha256 = (Get-FileHash -LiteralPath $runnerPath -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    foreach ($key in $SpecificMetadata.Keys) {
        if ($key -ne 'id') { $package[$key] = $SpecificMetadata[$key] }
    }
    return $package
}

$packages = @(
    (New-Package 'agents/android/armeabi-v7a/testtools-agent' 'android' 'armv7' ([ordered]@{ id = 'armeabi-v7a'
        android = [ordered]@{ abi = 'armeabi-v7a'; minimum_api = 23; stl = 'c++_static'; ndk = $AndroidNdkVersion }
    }))
    (New-Package 'agents/android/arm64-v8a/testtools-agent' 'android' 'aarch64' ([ordered]@{ id = 'arm64-v8a'
        android = [ordered]@{ abi = 'arm64-v8a'; minimum_api = 23; stl = 'c++_static'; ndk = $AndroidNdkVersion }
    }))
    (New-Package 'agents/linux/aarch64/glibc-2.31/testtools-agent' 'linux' 'aarch64' ([ordered]@{ id = 'aarch64/glibc-2.31'
        linux = [ordered]@{ libc = 'glibc'; minimum_version = '2.31'; toolchain = $LinuxToolchainId }
    }))
)

$manifest = [ordered]@{
    schema_version = 2
    product = 'TestTools'
    agent_version = $agentVersion
    protocol = [ordered]@{ minimum = [int]$protocolVersion; maximum = [int]$protocolVersion }
    build = [ordered]@{
        configuration = $Configuration
        build_id = $BuildId
        built_at_utc = $BuiltAtUtc
    }
    packages = $packages
}

$json = $manifest | ConvertTo-Json -Depth 8
[System.IO.File]::WriteAllText((Join-Path $bundleRoot 'manifest.json'), $json + [Environment]::NewLine, [System.Text.UTF8Encoding]::new($false))
Write-Host "Agent Bundle manifest written: $(Join-Path $bundleRoot 'manifest.json')"
