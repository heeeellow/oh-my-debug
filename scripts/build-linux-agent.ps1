param(
  [Parameter(Mandatory = $true)]
  [string]$ToolchainFile,
  [ValidateSet('armv7', 'aarch64')]
  [string]$Architecture = 'aarch64',
  [ValidateSet('Debug', 'Release')]
  [string]$Configuration = 'Release',
  [string]$OutputDirectory = '',
  [string]$CMakeProgram = 'cmake',
  [string]$NinjaProgram = 'ninja'
)

$ErrorActionPreference = 'Stop'
$repositoryRoot = Split-Path -Parent $PSScriptRoot
$sourceDirectory = Join-Path $repositoryRoot 'src\agent'
$buildDirectory = Join-Path $repositoryRoot "build\agent\linux-$Architecture-$Configuration"
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
  $OutputDirectory = Join-Path $repositoryRoot 'build\agents'
}
$artifactDirectory = Join-Path $OutputDirectory "linux\$Architecture"

if (-not (Test-Path -LiteralPath $ToolchainFile -PathType Leaf)) {
  throw "Linux ARM toolchain file not found: $ToolchainFile"
}

& $CMakeProgram -S $sourceDirectory -B $buildDirectory -G Ninja `
  "-DCMAKE_TOOLCHAIN_FILE=$ToolchainFile" `
  "-DCMAKE_MAKE_PROGRAM=$NinjaProgram" `
  "-DCMAKE_BUILD_TYPE=$Configuration" `
  '-DTESTTOOLS_BUILD_TESTS=OFF'
if ($LASTEXITCODE -ne 0) { throw 'Linux Agent configure failed' }

& $CMakeProgram --build $buildDirectory --parallel
if ($LASTEXITCODE -ne 0) { throw 'Linux Agent build failed' }

& $CMakeProgram -E make_directory $artifactDirectory
& $CMakeProgram -E copy_if_different `
  (Join-Path $buildDirectory 'testtools-agent') `
  (Join-Path $artifactDirectory 'testtools-agent')

Write-Host "Linux Agent ready: $artifactDirectory\testtools-agent"
