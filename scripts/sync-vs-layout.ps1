[CmdletBinding()]
param([switch]$Check)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repositoryRoot = Split-Path -Parent $PSScriptRoot
$encoding = [System.Text.UTF8Encoding]::new($false)

function Escape-Xml([string]$Value) {
    [System.Security.SecurityElement]::Escape($Value)
}

function Stable-Id([string]$Name) {
    $hash = [System.Security.Cryptography.SHA256]::Create()
    try {
        $bytes = $hash.ComputeHash($encoding.GetBytes('TestTools/VS/' + $Name))
        [guid]::new([byte[]]$bytes[0..15]).ToString('B').ToUpperInvariant()
    } finally { $hash.Dispose() }
}

function Sorted-Names($Values) {
    [string[]]$names = @($Values)
    [array]::Sort($names, [System.StringComparer]::Ordinal)
    $names
}

function Save-Layout([string]$Name, [System.Text.StringBuilder]$Text) {
    $path = Join-Path $repositoryRoot $Name
    $expected = $Text.ToString().Replace("`r`n", "`n")
    $actual = if (Test-Path -LiteralPath $path) {
        [System.IO.File]::ReadAllText($path).Replace("`r`n", "`n")
    } else { '' }
    if ($expected -ceq $actual) { return }
    if ($Check) { throw "$Name is out of date. Run scripts/sync-vs-layout.ps1." }
    [System.IO.File]::WriteAllText($path, $expected, $encoding)
    Write-Host "Updated $Name"
}

function Source-Filter([string]$Path) {
    $normalized = $Path.Replace('\', '/')
    $parts = $normalized.Split('/')
    if ($normalized -eq 'src/main.cpp') { return '应用入口' }
    if ($parts[0] -eq 'third_party') { return '第三方依赖\' + $parts[1] }
    if ($parts[0] -ne 'src') { throw "No VS filter rule for $Path" }
    switch ($parts[1]) {
        'application' {
            $names = @{ agent='Agent'; device='设备'; serial='串口'; support='通用'; terminal='终端' }
            if ($names.ContainsKey($parts[2])) { return '应用层\' + $names[$parts[2]] }
        }
        'core' {
            $names = @{ device='设备'; protocol='协议'; serial='串口'; support='通用' }
            if ($names.ContainsKey($parts[2])) { return '核心层\' + $names[$parts[2]] }
        }
        'adapters' {
            $names = @{ adb='ADB'; agent='Agent'; device='设备工具'; serial='本地串口' }
            if ($names.ContainsKey($parts[2])) { return '适配器\' + $names[$parts[2]] }
        }
        'diagnostics' { return '诊断' }
        'ui' {
            $names = @{ pages='页面'; widgets='组件'; resources='资源' }
            if ($names.ContainsKey($parts[2])) { return '界面\' + $names[$parts[2]] }
            return '界面\主窗口与主题'
        }
    }
    throw "No VS filter rule for $Path"
}

[xml]$project = Get-Content -LiteralPath (Join-Path $repositoryRoot 'TestTools.vcxproj') -Raw
$ns = [System.Xml.XmlNamespaceManager]::new($project.NameTable)
$ns.AddNamespace('m', $project.DocumentElement.NamespaceURI)
$types = @('QtMoc', 'ClCompile', 'ClInclude', 'QtRcc', 'None')
$items = @($project.SelectNodes('/m:Project/m:ItemGroup/*[@Include]', $ns) |
    Where-Object { $_.LocalName -in $types })
$assignments = @{}
$filters = @{}
foreach ($item in $items) {
    $include = $item.GetAttribute('Include')
    if (-not (Test-Path -LiteralPath (Join-Path $repositoryRoot $include) -PathType Leaf)) {
        throw "Missing project file: $include"
    }
    $key = $item.LocalName + ':' + $include
    if ($assignments.ContainsKey($key)) { throw "Duplicate project item: $key" }
    $filter = Source-Filter $include
    $assignments[$key] = $filter
    for ($parent = $filter; $parent; $parent = [System.IO.Path]::GetDirectoryName($parent)) {
        $filters[$parent] = Stable-Id ('filter/' + $parent)
    }
}
$text = [System.Text.StringBuilder]::new()
[void]$text.AppendLine('<?xml version="1.0" encoding="utf-8"?>')
[void]$text.AppendLine('<Project ToolsVersion="4.0" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">')
[void]$text.AppendLine('  <ItemGroup>')
foreach ($filter in (Sorted-Names $filters.Keys)) {
    [void]$text.AppendLine(('    <Filter Include="{0}"><UniqueIdentifier>{1}</UniqueIdentifier></Filter>' -f (Escape-Xml $filter), $filters[$filter]))
}
[void]$text.AppendLine('  </ItemGroup>')
foreach ($type in $types) {
    $paths = @($items | Where-Object { $_.LocalName -eq $type } | ForEach-Object { $_.GetAttribute('Include') })
    if (-not $paths.Count) { continue }
    [void]$text.AppendLine('  <ItemGroup>')
    foreach ($path in (Sorted-Names $paths)) {
        [void]$text.AppendLine(('    <{0} Include="{1}"><Filter>{2}</Filter></{0}>' -f $type, (Escape-Xml $path), (Escape-Xml $assignments[($type + ':' + $path)])))
    }
    [void]$text.AppendLine('  </ItemGroup>')
}
[void]$text.AppendLine('</Project>')
Save-Layout 'TestTools.vcxproj.filters' $text

# Solution items are navigation only: the GUI remains the sole build/startup project.
[xml]$solution = Get-Content -LiteralPath (Join-Path $repositoryRoot 'TestTools.slnx') -Raw
$projects = @($solution.SelectNodes('//Project'))
if ($projects.Count -ne 1 -or $projects[0].GetAttribute('Path') -ne 'TestTools.vcxproj') {
    throw 'Review solution layout rules before adding another build project.'
}
$folders = @{}
function Add-SolutionFile([string]$Folder, [string]$Path) {
    if (-not $folders.ContainsKey($Folder)) { $folders[$Folder] = [System.Collections.Generic.List[string]]::new() }
    $folders[$Folder].Add($Path.Replace('\', '/'))
    for ($parent = [System.IO.Path]::GetDirectoryName($Folder); $parent; $parent = [System.IO.Path]::GetDirectoryName($parent)) {
        if (-not $folders.ContainsKey($parent)) { $folders[$parent] = [System.Collections.Generic.List[string]]::new() }
    }
}
foreach ($path in @('README.md', 'AGENTS.md')) { Add-SolutionFile '文档' $path }
foreach ($path in @('.clang-format', '.editorconfig', '.gitattributes', '.gitignore', 'VERSION.txt')) {
    Add-SolutionFile '工程配置' $path
}
foreach ($directory in @('docs', 'scripts', 'tests', 'src/agent')) {
    foreach ($file in (Get-ChildItem -LiteralPath (Join-Path $repositoryRoot $directory) -Recurse -File)) {
        $path = $file.FullName.Substring($repositoryRoot.Length + 1).Replace('\', '/')
        if ($path -match '/(build|out|obj|x64)/') { continue }
        if ($directory -eq 'docs' -and $file.Extension -eq '.md') {
            $folder = if ($path.StartsWith('docs/features/')) { '文档\功能契约' }
                elseif ($path.StartsWith('docs/decisions/')) { '文档\设计决策' }
                elseif ($path.StartsWith('docs/design/')) { '文档\设计参考' } else { '文档' }
        } elseif ($directory -eq 'scripts' -and $file.Extension -in @('.ps1', '.sh')) {
            $folder = '构建与验证'
        } elseif ($directory -eq 'tests' -and $file.Extension -in @('.cpp', '.h', '.txt')) {
            $folder = if ($path.StartsWith('tests/ui/')) { '测试\原生界面' } else { '测试\核心与应用' }
        } elseif ($directory -eq 'src/agent' -and $file.Extension -in @('.cpp', '.h', '.txt')) {
            $folder = if ($file.Name -match '_test\.cpp$|^fake_agent_') { '板端 Agent\本机夹具' } else { '板端 Agent' }
        } else { continue }
        Add-SolutionFile $folder $path
    }
}
$text = [System.Text.StringBuilder]::new()
[void]$text.AppendLine('<Solution>')
[void]$text.AppendLine('  <Configurations>')
[void]$text.AppendLine('    <Platform Name="x64" />')
[void]$text.AppendLine('  </Configurations>')
[void]$text.AppendLine(('  ' + $projects[0].OuterXml))
foreach ($folder in (Sorted-Names $folders.Keys)) {
    $id = (Stable-Id ('solution/' + $folder)).Trim('{}').ToLowerInvariant()
    [void]$text.AppendLine(('  <Folder Name="/{0}/" Id="{1}">' -f (Escape-Xml $folder.Replace('\', '/')), $id))
    foreach ($path in (Sorted-Names $folders[$folder])) {
        [void]$text.AppendLine(('    <File Path="{0}" />' -f (Escape-Xml $path)))
    }
    [void]$text.AppendLine('  </Folder>')
}
[void]$text.AppendLine('</Solution>')
Save-Layout 'TestTools.slnx' $text
Write-Host "VS layout check passed: $($items.Count) project files, $($filters.Count) filters, $($folders.Count) solution folders."
