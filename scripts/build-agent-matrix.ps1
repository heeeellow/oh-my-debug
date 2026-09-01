[CmdletBinding()]
param(
    [string]$NdkRoot = '',
    [string]$WslDistribution = 'Ubuntu-24.04',
    [string]$LinuxToolchainRoot = '',
    [string]$CMakeProgram = 'cmake',
    [string]$NinjaProgram = 'ninja',
    [ValidateSet('Release')]
    [string]$Configuration = 'Release',
    [string]$OutputDirectory = ''
)

$ErrorActionPreference = 'Stop'
$repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
Push-Location $repositoryRoot
try {
    function Invoke-WslText {
        param([Parameter(Mandatory = $true)][string[]]$ArgumentList)
        $result = & wsl.exe -d $WslDistribution -- @ArgumentList
        if ($LASTEXITCODE -ne 0) { throw "WSL command failed: $($ArgumentList -join ' ')" }
        return (($result -join "`n").Trim())
    }

    function ConvertTo-WslPath {
        param([Parameter(Mandatory = $true)][string]$WindowsPath)
        $slashPath = $WindowsPath.Replace('\', '/')
        return Invoke-WslText @('wslpath', '-a', '-u', $slashPath)
    }

    if ([string]::IsNullOrWhiteSpace($NdkRoot)) {
        foreach ($candidate in @($env:TestToolsNdkRoot, $env:ANDROID_NDK_ROOT, $env:ANDROID_NDK_HOME)) {
            if (-not [string]::IsNullOrWhiteSpace($candidate) -and (Test-Path -LiteralPath $candidate -PathType Container)) {
                $NdkRoot = $candidate
                break
            }
        }
    }
    if ([string]::IsNullOrWhiteSpace($NdkRoot)) {
        $sdkRoot = Join-Path $env:LOCALAPPDATA 'Android\Sdk\ndk'
        $NdkRoot = Get-ChildItem -LiteralPath $sdkRoot -Directory -ErrorAction SilentlyContinue |
            Sort-Object Name -Descending | Select-Object -First 1 -ExpandProperty FullName
    }
    if ([string]::IsNullOrWhiteSpace($NdkRoot) -or -not (Test-Path -LiteralPath $NdkRoot -PathType Container)) {
        throw 'Android NDK not found. Pass -NdkRoot or set TestToolsNdkRoot/ANDROID_NDK_ROOT.'
    }

    $ndkProperties = Join-Path $NdkRoot 'source.properties'
    if (-not (Test-Path -LiteralPath $ndkProperties -PathType Leaf)) { throw "NDK source.properties not found: $NdkRoot" }
    $ndkVersionMatch = [regex]::Match((Get-Content -LiteralPath $ndkProperties -Raw), 'Pkg.Revision\s*=\s*(?<version>[^\r\n]+)')
    if (-not $ndkVersionMatch.Success) { throw 'Unable to determine Android NDK revision.' }
    $androidNdkVersion = $ndkVersionMatch.Groups['version'].Value.Trim()

    if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
        $OutputDirectory = Join-Path $repositoryRoot 'build\agent-bundle'
    } else {
        $OutputDirectory = [System.IO.Path]::GetFullPath($OutputDirectory)
    }
    $buildRoot = [System.IO.Path]::GetFullPath((Join-Path $repositoryRoot 'build'))
    $buildPrefix = $buildRoot.TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
    if (-not $OutputDirectory.StartsWith($buildPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw 'OutputDirectory must remain below the repository build directory.'
    }
    if (Test-Path -LiteralPath $OutputDirectory) {
        Remove-Item -LiteralPath $OutputDirectory -Recurse -Force
    }
    New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null

    $sourceRoots = @('src\agent', 'src\core\protocol', 'src\core\support')
    $sourceFiles = foreach ($sourceRoot in $sourceRoots) {
        Get-ChildItem -LiteralPath (Join-Path $repositoryRoot $sourceRoot) -Recurse -File |
            Where-Object { $_.Extension -in @('.h', '.cpp') }
    }
    $sourceFingerprintLines = foreach ($sourceFile in ($sourceFiles | Sort-Object FullName)) {
        $relative = $sourceFile.FullName.Substring($repositoryRoot.Length + 1).Replace('\', '/')
        $hash = (Get-FileHash -LiteralPath $sourceFile.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        "$relative=$hash"
    }
    $buildInputs = @(
        "configuration=$Configuration",
        "android_ndk=$androidNdkVersion",
        'linux_toolchain=debian-bullseye-gcc-10.2.1-glibc-2.31'
    ) + $sourceFingerprintLines
    $buildInputBytes = [System.Text.Encoding]::UTF8.GetBytes(($buildInputs -join "`n"))
    $buildInputHash = [System.Security.Cryptography.SHA256]::Create().ComputeHash($buildInputBytes)
    $buildId = 'm1-' + (($buildInputHash | ForEach-Object { $_.ToString('x2') }) -join '').Substring(0, 16)

    $androidScript = Join-Path $PSScriptRoot 'build-android-agent.ps1'
    foreach ($abi in @('armeabi-v7a', 'arm64-v8a')) {
        & $androidScript -NdkRoot $NdkRoot -Abi $abi -Configuration $Configuration -OutputDirectory $OutputDirectory `
            -CMakeProgram $CMakeProgram -NinjaProgram $NinjaProgram
        if ($LASTEXITCODE -ne 0) { throw "Android Agent build failed: $abi" }
    }

    $wslHome = Invoke-WslText @('printenv', 'HOME')
    if ([string]::IsNullOrWhiteSpace($LinuxToolchainRoot)) {
        $LinuxToolchainRoot = "$wslHome/.local/share/testtools/toolchains/debian-bullseye-aarch64-glibc-2.31"
    }
    $wslRepositoryRoot = ConvertTo-WslPath $repositoryRoot
    $wslOutputDirectory = ConvertTo-WslPath $OutputDirectory
    $wslBuildDirectory = "$wslHome/.cache/testtools-agent-matrix/$buildId/linux-aarch64-$Configuration"
    $wslToolchainFile = "$wslRepositoryRoot/toolchains/linux-aarch64-glibc-2.31.cmake"
    $linuxScript = ConvertTo-WslPath (Join-Path $PSScriptRoot 'build-linux-agent-aarch64.sh')
    $linuxOutput = "$wslOutputDirectory/agents/linux/aarch64/glibc-2.31/testtools-agent"
    & wsl.exe -d $WslDistribution -- bash $linuxScript $wslRepositoryRoot/src/agent $linuxOutput $wslBuildDirectory $LinuxToolchainRoot $wslToolchainFile
    if ($LASTEXITCODE -ne 0) { throw 'Linux aarch64 Agent build failed.' }

    $manifestScript = Join-Path $PSScriptRoot 'new-agent-bundle-manifest.ps1'
    if (-not (Test-Path -LiteralPath $manifestScript -PathType Leaf)) {
        throw "Manifest generator not found: $manifestScript"
    }
    & $manifestScript -BundleDirectory $OutputDirectory -BuildId $buildId `
        -BuiltAtUtc ([DateTime]::UtcNow.ToString('o')) -Configuration $Configuration `
        -AndroidNdkVersion $androidNdkVersion -LinuxToolchainId 'debian-bullseye-gcc-10.2.1-glibc-2.31'
    & (Join-Path $PSScriptRoot 'validate-agent-bundle.ps1') -BundleDirectory $OutputDirectory
    Write-Host "Agent matrix ready: $OutputDirectory"
} finally {
    Pop-Location
}
