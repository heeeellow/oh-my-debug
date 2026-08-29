param(
    [Parameter(Mandatory = $true)]
    [string]$NdkRoot,
    [ValidateSet('armeabi-v7a', 'arm64-v8a', 'x86', 'x86_64')]
    [string]$Abi = 'armeabi-v7a',
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [string]$CMakeProgram = 'cmake',
    [string]$NinjaProgram = 'ninja'
)

$ErrorActionPreference = 'Stop'
$repositoryRoot = Split-Path -Parent $PSScriptRoot
$sourceDirectory = Join-Path $repositoryRoot 'test\agent'
$buildDirectory = Join-Path $sourceDirectory "out\android-$Abi"
$toolchain = Join-Path $NdkRoot 'build\cmake\android.toolchain.cmake'
$applicationOutput = Join-Path $repositoryRoot "test\x64\$Configuration\agents\android\$Abi"

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
