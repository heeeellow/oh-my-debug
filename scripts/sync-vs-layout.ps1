[CmdletBinding()]
param([switch]$Check)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$encoding = [Text.UTF8Encoding]::new($false)
function Xml([string]$s) { [Security.SecurityElement]::Escape($s) }
function Id([string]$s) {
    $hash = [Security.Cryptography.SHA256]::Create()
    try { [guid]::new([byte[]]$hash.ComputeHash($encoding.GetBytes('OhMyDebug/VS/' + $s))[0..15]).ToString('B').ToUpperInvariant() }
    finally { $hash.Dispose() }
}
function Sorted($values) {
    [string[]]$names = @($values)
    [array]::Sort($names, [StringComparer]::Ordinal)
    $names
}
function Save([string]$name, [Text.StringBuilder]$text) {
    $path = Join-Path $root $name
    $expected = $text.ToString().Replace("`r`n", "`n")
    $actual = if ([IO.File]::Exists($path)) { [IO.File]::ReadAllText($path).Replace("`r`n", "`n") } else { '' }
    if ($expected -ceq $actual) { return }
    if ($Check) { throw "$name is out of date. Run scripts/sync-vs-layout.ps1." }
    [IO.File]::WriteAllText($path, $expected, $encoding)
    Write-Host "Updated $name"
}
[xml]$project = [IO.File]::ReadAllText((Join-Path $root 'OhMyDebug.vcxproj'))
$ns = [Xml.XmlNamespaceManager]::new($project.NameTable)
$ns.AddNamespace('m', $project.DocumentElement.NamespaceURI)
$types = @('QtMoc', 'ClCompile', 'ClInclude', 'QtRcc', 'ResourceCompile', 'Image', 'None')
$items = @($project.SelectNodes('/m:Project/m:ItemGroup/*[@Include]', $ns) | Where-Object { $_.LocalName -in $types })
$groups = @{}
$assignments = @{}
foreach ($item in $items) {
    $path = $item.GetAttribute('Include').Replace('/', '\')
    if (-not [IO.File]::Exists((Join-Path $root $path))) { throw "Missing project file: $path" }
    $key = $item.LocalName + ':' + $path
    if ($assignments.ContainsKey($key)) { throw "Duplicate project item: $key" }
    $parent = [IO.Path]::GetDirectoryName($path)
    if (-not $parent) { throw "Project file needs a source directory: $path" }
    $assignments[$key] = $parent
    for ($folder = $parent; $folder; $folder = [IO.Path]::GetDirectoryName($folder)) { $groups[$folder] = Id $folder }
}
$text = [Text.StringBuilder]::new()
[void]$text.AppendLine('<?xml version="1.0" encoding="utf-8"?>')
[void]$text.AppendLine('<Project ToolsVersion="4.0" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">')
[void]$text.AppendLine('  <ItemGroup>')
foreach ($folder in (Sorted $groups.Keys)) {
    [void]$text.AppendLine(('    <Filter Include="{0}"><UniqueIdentifier>{1}</UniqueIdentifier></Filter>' -f (Xml $folder), $groups[$folder]))
}
[void]$text.AppendLine('  </ItemGroup>')
foreach ($type in $types) {
    $paths = @($items | Where-Object { $_.LocalName -eq $type } | ForEach-Object { $_.GetAttribute('Include').Replace('/', '\') })
    if (-not $paths.Count) { continue }
    [void]$text.AppendLine('  <ItemGroup>')
    foreach ($path in (Sorted $paths)) {
        [void]$text.AppendLine(('    <{0} Include="{1}"><Filter>{2}</Filter></{0}>' -f $type, (Xml $path), (Xml $assignments[($type + ':' + $path)])))
    }
    [void]$text.AppendLine('  </ItemGroup>')
}
[void]$text.AppendLine('</Project>')
Save 'OhMyDebug.vcxproj.filters' $text
[xml]$solution = [IO.File]::ReadAllText((Join-Path $root 'OhMyDebug.slnx'))
$projects = @($solution.SelectNodes('//Project'))
if ($projects.Count -ne 1 -or $projects[0].GetAttribute('Path') -ne 'OhMyDebug.vcxproj') { throw 'Expected the desktop GUI as the sole solution project.' }
$text = [Text.StringBuilder]::new()
[void]$text.AppendLine('<Solution>')
[void]$text.AppendLine('  <Configurations><Platform Name="x64" /></Configurations>')
[void]$text.AppendLine(('  ' + $projects[0].OuterXml))
[void]$text.AppendLine(('  <Folder Name="/docs/" Id="{0}">' -f (Id 'solution/docs').Trim('{}').ToLowerInvariant()))
foreach ($path in @('README.md', 'LICENSE', 'docs/USAGE.md', 'docs/DEVELOPMENT.md', 'docs/ARCHITECTURE.md')) {
    if (-not [IO.File]::Exists((Join-Path $root $path))) { throw "Missing solution document: $path" }
    [void]$text.AppendLine(('    <File Path="{0}" />' -f (Xml $path)))
}
[void]$text.AppendLine('  </Folder>')
$agentPaths = @(Get-ChildItem -LiteralPath (Join-Path $root 'src/agent') -File |
    Where-Object { $_.Extension -in @('.cpp', '.h') -or $_.Name -eq 'CMakeLists.txt' } |
    ForEach-Object { 'src/agent/' + $_.Name })
$agentPaths += @('src/core/protocol/agent_version.h', 'tests/agent/agent_service_test.cpp',
                 'scripts/build-agent-matrix.ps1', 'docs/AGENT_PROTOCOL_V1.md')
[void]$text.AppendLine(('  <Folder Name="/agent/" Id="{0}">' -f (Id 'solution/agent').Trim('{}').ToLowerInvariant()))
foreach ($path in (Sorted $agentPaths)) {
    if (-not [IO.File]::Exists((Join-Path $root $path))) { throw "Missing Agent navigation file: $path" }
    [void]$text.AppendLine(('    <File Path="{0}" />' -f (Xml $path)))
}
[void]$text.AppendLine('  </Folder>')
[void]$text.AppendLine('</Solution>')
Save 'OhMyDebug.slnx' $text
Write-Host "VS layout check passed: $($items.Count) project files; 5 document shortcuts; $($agentPaths.Count) Agent navigation files."
