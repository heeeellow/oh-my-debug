[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$VersionFile,
    [Parameter(Mandatory = $true)][string]$OutputFile
)
$ErrorActionPreference = 'Stop'
$version = [IO.File]::ReadAllText($VersionFile).Trim()
if ($version -notmatch '^(\d+)\.(\d+)\.(\d+)$') { throw 'VERSION.txt must contain major.minor.patch.' }
$parts = @([int]$Matches[1], [int]$Matches[2], [int]$Matches[3], 0)
if (@($parts | Where-Object { $_ -gt 65535 }).Count) { throw 'Version components exceed Windows resource limits.' }
$content = "#pragma once`n#define OMD_VERSION `"$version`"`n#define OMD_VERSION_COMMA $($parts -join ',')`n"
$directory = Split-Path -Parent ([IO.Path]::GetFullPath($OutputFile))
[void][IO.Directory]::CreateDirectory($directory)
if (-not [IO.File]::Exists($OutputFile) -or [IO.File]::ReadAllText($OutputFile) -cne $content) {
    [IO.File]::WriteAllText($OutputFile, $content, [Text.UTF8Encoding]::new($false))
}
