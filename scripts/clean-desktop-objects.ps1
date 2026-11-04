[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'
$repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$intermediateRoot = [System.IO.Path]::GetFullPath((Join-Path $repositoryRoot 'build\intermediate\desktop\x64'))
$objectDirectory = [System.IO.Path]::GetFullPath((Join-Path $intermediateRoot $Configuration))
if (-not $objectDirectory.StartsWith($intermediateRoot + [System.IO.Path]::DirectorySeparatorChar,
        [System.StringComparison]::OrdinalIgnoreCase)) {
    throw 'Object directory escaped the desktop intermediate directory.'
}
if (-not (Test-Path -LiteralPath $objectDirectory -PathType Container)) { return }

[xml]$project = Get-Content -LiteralPath (Join-Path $repositoryRoot 'OhMyDebug.vcxproj') -Raw
$expected = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
foreach ($item in $project.SelectNodes('//*[@Include]')) {
    if ($item.LocalName -notin @('ClCompile', 'QtMoc', 'QtRcc')) { continue }
    $stem = [System.IO.Path]::GetFileNameWithoutExtension($item.GetAttribute('Include'))
    if ($item.LocalName -eq 'QtMoc') { $stem = 'moc_' + $stem }
    elseif ($item.LocalName -eq 'QtRcc') { $stem = 'qrc_' + $stem }
    [void]$expected.Add($stem + '.obj')
}
$removed = 0
foreach ($file in Get-ChildItem -LiteralPath $objectDirectory -File -Filter '*.obj') {
    if (-not $expected.Contains($file.Name)) {
        Remove-Item -LiteralPath $file.FullName
        Write-Output "Removed stale $Configuration object: $($file.Name)"
        $removed++
    }
}
Write-Output "Desktop $Configuration object cleanup: $removed stale objects removed."
