[CmdletBinding()]
param()
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
foreach ($dependency in @('iperf3', 'nlohmann')) {
    $directory = Join-Path $root "third_party/$dependency"
    $manifest = Join-Path $directory 'SHA256SUMS.txt'
    if (-not (Test-Path -LiteralPath $manifest -PathType Leaf)) { throw "Missing toolbox dependency manifest: $manifest" }
    $prefix = [IO.Path]::GetFullPath($directory).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    $checkedFiles = 0
    foreach ($line in Get-Content -LiteralPath $manifest) {
        if ([string]::IsNullOrWhiteSpace($line)) { continue }
        if ($line -notmatch '^(?<hash>[0-9a-f]{64})  (?<path>[^\r\n]+)$') { throw "Invalid dependency digest line: $dependency" }
        $expected = $Matches['hash']
        $path = [IO.Path]::GetFullPath((Join-Path $directory $Matches['path']))
        if (-not $path.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) { throw 'Dependency path escaped its directory' }
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing pinned dependency: $path" }
        $stream = [IO.File]::OpenRead($path)
        $sha256 = [Security.Cryptography.SHA256]::Create()
        try { $actual = [BitConverter]::ToString($sha256.ComputeHash($stream)).Replace('-', '').ToLowerInvariant() }
        finally { $sha256.Dispose(); $stream.Dispose() }
        if ($actual -ne $expected) { throw "Toolbox dependency digest mismatch: $path" }
        $checkedFiles++
    }
    if ($checkedFiles -eq 0) { throw "Empty dependency digest manifest: $dependency" }
}
Write-Host 'Toolbox dependencies valid: pinned iperf3, runtime, JSON header and notices match.'
