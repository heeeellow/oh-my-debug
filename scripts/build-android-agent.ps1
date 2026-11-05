param(
    [Parameter(Mandatory = $true)]
    [string]$NdkRoot,
    [ValidateSet('armeabi-v7a', 'arm64-v8a')]
    [string]$Abi = 'armeabi-v7a',
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [string]$OutputDirectory = '',
    [string]$CMakeProgram = 'cmake',
    [string]$NinjaProgram = 'ninja'
)

$ErrorActionPreference = 'Stop'
$repositoryRoot = Split-Path -Parent $PSScriptRoot
$sourceDirectory = Join-Path $repositoryRoot 'src\agent'
$buildDirectory = Join-Path $repositoryRoot "build\agent\android-$Abi-$Configuration"
$toolchain = Join-Path $NdkRoot 'build\cmake\android.toolchain.cmake'
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $repositoryRoot "build\windows\x64\$Configuration"
}
$applicationOutput = Join-Path $OutputDirectory "agents\android\$Abi"

if (-not (Test-Path -LiteralPath $toolchain -PathType Leaf)) {
    throw "Android NDK toolchain not found: $toolchain"
}

& $CMakeProgram -S $sourceDirectory -B $buildDirectory -G Ninja `
    "-DCMAKE_TOOLCHAIN_FILE=$toolchain" `
    "-DCMAKE_MAKE_PROGRAM=$NinjaProgram" `
    "-DANDROID_ABI=$Abi" `
    '-DANDROID_PLATFORM=android-23' `
    '-DANDROID_STL=c++_static' `
    "-DCMAKE_BUILD_TYPE=$Configuration"
if ($LASTEXITCODE -ne 0) { throw 'Android Agent configure failed' }

& $CMakeProgram --build $buildDirectory --parallel
if ($LASTEXITCODE -ne 0) { throw 'Android Agent build failed' }

& $CMakeProgram -E make_directory $applicationOutput
& $CMakeProgram -E copy_if_different `
    (Join-Path $buildDirectory 'testtools-agent') `
    (Join-Path $applicationOutput 'testtools-agent')

Write-Host "Android Agent ready: $applicationOutput\testtools-agent"
& $CMakeProgram -E copy_if_different (Join-Path $buildDirectory 'iperf3') (Join-Path $applicationOutput 'iperf3')
if ($LASTEXITCODE -ne 0) { throw 'Android iperf3 Runner copy failed' }
