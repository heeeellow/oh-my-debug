[CmdletBinding()]
param(
    [string]$RuntimeDirectory = '',
    [Parameter(Mandatory = $true)][string]$QtSourceDirectory,
    [Parameter(Mandatory = $true)][string]$VcRuntimeDirectory,
    [Parameter(Mandatory = $true)][string]$CygwinSourceArchive,
    [string]$OutputDirectory = '',
    [switch]$AllowDirty
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $RuntimeDirectory) { $RuntimeDirectory = Join-Path $root 'build/windows/x64/Release' }
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $root 'build/packages' }
$runtime = (Resolve-Path -LiteralPath $RuntimeDirectory).Path
$qtSource = (Resolve-Path -LiteralPath $QtSourceDirectory).Path
$vcRuntime = (Resolve-Path -LiteralPath $VcRuntimeDirectory).Path
$cygwinSource = (Resolve-Path -LiteralPath $CygwinSourceArchive).Path
$cygwinSourceManifest = Get-Content -LiteralPath (Join-Path $root 'third_party/iperf3/CYGWIN-SOURCE.json') -Raw | ConvertFrom-Json
if ($cygwinSourceManifest.schema -ne 'ohmydebug.cygwin-source/v1' -or
    (Get-FileHash -LiteralPath $cygwinSource -Algorithm SHA256).Hash -ne $cygwinSourceManifest.sha256 -or
    (Get-FileHash -LiteralPath (Join-Path $root 'third_party/iperf3/windows/cygwin1.dll') -Algorithm SHA256).Hash -ne $cygwinSourceManifest.runtime_sha256) {
    throw 'Cygwin corresponding source archive or runtime does not match the pinned source manifest.'
}
$vcFiles = @('concrt140.dll','msvcp140.dll','msvcp140_1.dll','msvcp140_2.dll','msvcp140_atomic_wait.dll','msvcp140_codecvt_ids.dll','vcruntime140.dll','vcruntime140_1.dll','vcruntime140_threads.dll')
foreach ($file in $vcFiles) {
    $path = Join-Path $vcRuntime $file
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing redistributable runtime: $file" }
    $bytes = [IO.File]::ReadAllBytes($path)
    $pe = [BitConverter]::ToInt32($bytes, 60)
    if ([BitConverter]::ToUInt16($bytes, $pe + 4) -ne 0x8664) { throw "Expected x64 runtime: $file" }
}
$vcVersion = (Get-Item -LiteralPath (Join-Path $vcRuntime 'vcruntime140.dll')).VersionInfo.FileVersion
if ([version]$vcVersion -lt [version]'14.44.35211.0') { throw 'MSVC runtime is older than the supported product toolchain.' }
$version = [IO.File]::ReadAllText((Join-Path $root 'VERSION.txt')).Trim()
if ($version -notmatch '^\d+\.\d+\.\d+$') { throw 'Invalid VERSION.txt' }
$appInfo = [IO.File]::ReadAllText((Join-Path $root 'src/app_info.h'))
$companyMatch = [regex]::Match($appInfo, '(?m)^#define OMD_COMPANY_NAME "([^"]+)"')
$developerMatch = [regex]::Match($appInfo, '(?m)^#define OMD_DEVELOPER_NAME "([^"]+)"')
$copyrightMatch = [regex]::Match($appInfo, '(?m)^#define OMD_COPYRIGHT "([^"]+)" OMD_COMPANY_NAME')
if (-not $companyMatch.Success -or -not $developerMatch.Success -or -not $copyrightMatch.Success) {
    throw 'Application copyright metadata is missing from src/app_info.h.'
}
$copyrightOwner = $companyMatch.Groups[1].Value
$developer = $developerMatch.Groups[1].Value
$copyrightNotice = $copyrightMatch.Groups[1].Value + $copyrightOwner
$exe = Join-Path $runtime 'OhMyDebug.exe'
$info = (Get-Item -LiteralPath $exe).VersionInfo
if ($info.ProductVersion -ne $version -or $info.ProductName -ne 'Oh My Debug' -or
    $info.OriginalFilename -ne 'OhMyDebug.exe' -or $info.IsDebug -or
    $info.CompanyName -ne $copyrightOwner -or $info.LegalCopyright -ne $copyrightNotice) {
    throw 'Build the current Release version before packaging.'
}
$qtVersion = ([version](Get-Item -LiteralPath (Join-Path $runtime 'Qt6Core.dll')).VersionInfo.ProductVersion).ToString(3)
if ($qtVersion -ne '6.11.2') { throw "Expected Qt 6.11.2 runtime, got $qtVersion" }
$qtConfig = Join-Path $qtSource 'qtbase/.cmake.conf'
if (-not (Test-Path -LiteralPath $qtConfig) -or
    [IO.File]::ReadAllText($qtConfig) -notmatch 'QT_REPO_MODULE_VERSION "6\.11\.2"') {
    throw 'QtSourceDirectory must contain the Qt 6.11.2 sources, including qtbase and LICENSES.'
}
Push-Location $root
try {
    $revision = (& git rev-parse HEAD).Trim()
    if ($LASTEXITCODE -ne 0) { throw 'Cannot read source revision.' }
    $dirty = [bool](& git status --porcelain)
    if ($dirty -and -not $AllowDirty) { throw 'Source tree is dirty. Commit first, or use -AllowDirty for a local preview.' }
} finally { Pop-Location }
$bundle = Join-Path $runtime 'agent-bundle'
& (Join-Path $PSScriptRoot 'validate-agent-bundle.ps1') -BundleDirectory $bundle
if (-not $?) { throw 'Agent Bundle validation failed.' }
$bundleManifest = Get-Content -LiteralPath (Join-Path $bundle 'manifest.json') -Raw | ConvertFrom-Json
$suffix = if ($dirty) { '-preview' } else { '' }
$name = "OhMyDebug-$version-windows-x64$suffix"
$output = [IO.Path]::GetFullPath($OutputDirectory)
$stage = Join-Path $output $name
$archive = Join-Path $output "$name.zip"
if ((Test-Path -LiteralPath $stage) -or (Test-Path -LiteralPath $archive)) {
    throw 'Output already exists. Choose a new OutputDirectory; existing files are never deleted.'
}
[void](New-Item -ItemType Directory -Path $stage -Force)
function Copy-File([string]$source, [string]$relative) {
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { throw "Required file is missing: $source" }
    $destination = Join-Path $stage $relative
    [void](New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force)
    Copy-Item -LiteralPath $source -Destination $destination
}
foreach ($file in @('OhMyDebug.exe','Qt6Core.dll','Qt6Gui.dll','Qt6Widgets.dll','Qt6Svg.dll','Qt6Network.dll',
                    'platforms/qwindows.dll','imageformats/qsvg.dll','iconengines/qsvgicon.dll')) {
    Copy-File (Join-Path $runtime $file) $file
}
foreach ($file in $vcFiles) { Copy-File (Join-Path $vcRuntime $file) $file }
foreach ($file in @('dxcompiler.dll','dxil.dll','generic/qtuiotouchplugin.dll','imageformats/qgif.dll','imageformats/qico.dll',
                    'imageformats/qjpeg.dll','networkinformation/qnetworklistmanager.dll',
                    'styles/qmodernwindowsstyle.dll','tls/qcertonlybackend.dll','tls/qschannelbackend.dll')) {
    if (Test-Path -LiteralPath (Join-Path $runtime $file)) { Copy-File (Join-Path $runtime $file) $file }
}
Copy-File (Join-Path $bundle 'manifest.json') 'agent-bundle/manifest.json'
foreach ($package in $bundleManifest.packages) {
    foreach ($file in $package.files) {
        $relative = "$($package.root)/$($file.path)"
        Copy-File (Join-Path $bundle $relative) "agent-bundle/$relative"
    }
}
foreach ($directory in @('platform-tools','scrcpy')) {
    Get-ChildItem -LiteralPath (Join-Path $root "third_party/$directory") -Recurse -File | ForEach-Object {
        $relative = $_.FullName.Substring((Join-Path $root "third_party/$directory").Length + 1)
        $source = Join-Path $runtime "$directory/$relative"
        if ((Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash -ne
            (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash) { throw "Bundled tool differs from source: $relative" }
        Copy-File $source "$directory/$relative"
    }
}
foreach ($file in @('LICENSE','docs/USAGE.md','docs/THIRD_PARTY.md','docs/features/TOOLBOX.md',"docs/releases/$version.md")) { Copy-File (Join-Path $root $file) $file }
& (Join-Path $PSScriptRoot 'validate-toolbox-dependencies.ps1')
foreach ($file in @('iperf3.exe','cygwin1.dll')) {
    $source = Join-Path $runtime "iperf3/$file"
    if ((Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash -ne
        (Get-FileHash -LiteralPath (Join-Path $root "third_party/iperf3/windows/$file") -Algorithm SHA256).Hash) { throw "Bundled iperf3 differs from source: $file" }
    Copy-File $source "iperf3/$file"
}
foreach ($dependency in @('asio','CSerialPort','tiny-process-library','libvterm','phosphor-icons','iperf3','nlohmann')) {
    Get-ChildItem -LiteralPath (Join-Path $root "third_party/$dependency") -File |
        Where-Object { $_.Name -match '^(LICENSE|COPYING|CYGWIN|VERSION|SHA256)' } |
        ForEach-Object { Copy-File $_.FullName "licenses/$dependency/$($_.Name)" }
}
foreach ($file in @('iperf-3.20.tar.gz','CMakeLists.txt','iperf_config.h.in')) { Copy-File (Join-Path $root "third_party/iperf3/$file") "licenses/iperf3/$file" }
Copy-File $cygwinSource "licenses/iperf3/$($cygwinSourceManifest.archive)"
foreach ($file in @('LGPL-3.0-only.txt','GPL-3.0-only.txt','GPL-2.0-only.txt','Qt-GPL-exception-1.0.txt','BSD-3-Clause.txt')) {
    Copy-File (Join-Path $qtSource "LICENSES/$file") "licenses/Qt/LICENSES/$file"
}
# Preserve notices from the modules we ship, including their bundled libraries.
foreach ($module in @('qtbase','qtsvg')) {
    Get-ChildItem -LiteralPath (Join-Path $qtSource $module) -Recurse -File |
        Where-Object { $_.Name -match '(?i)(license|copying|copyright|notice|authors|patents|^FTL\.TXT$|^qt_attribution\.json$)' } |
        ForEach-Object { Copy-File $_.FullName ('licenses/Qt/' + $_.FullName.Substring($qtSource.Length + 1)) }
}
$utf8 = [Text.UTF8Encoding]::new($false)
$readme = @"
# Oh My Debug $version

1. 将整个 ZIP 完整解压到一个新文件夹。
2. 双击 OhMyDebug.exe 即可使用，无需安装 Qt、Visual Studio 或配置 PATH。
3. 保留所有 DLL 和子目录，不要只复制主程序，也不要直接从压缩包内运行。

## 支持环境

Windows 10（1809 起）或 Windows 11，x64。所需 Qt、MSVC 运行库、ADB、投屏和测速工具均已随包提供。
连接硬件时，电脑仍需能识别对应 COM 或 ADB 设备；本包不自动安装硬件驱动。
程序设置保存在当前用户目录，日志按用户选择保存。

[使用手册](docs/USAGE.md) · [更新说明](docs/releases/$version.md) · [许可](LICENSE) · [第三方声明](docs/THIRD_PARTY.md)

版本、源码提交与文件摘要见 manifest.json。$copyrightNotice。
开发者：$developer。
"@
[IO.File]::WriteAllText((Join-Path $stage 'README.md'), $readme, $utf8)
$notices = [IO.File]::ReadAllText((Join-Path $stage 'docs/THIRD_PARTY.md'))
foreach ($dependency in @('asio','CSerialPort','tiny-process-library','libvterm','phosphor-icons','iperf3','nlohmann')) {
    $notices = $notices.Replace("../third_party/$dependency/", "../licenses/$dependency/")
}
$notices = $notices.Replace('../third_party/platform-tools/', '../platform-tools/').Replace('../third_party/scrcpy/', '../scrcpy/')
$notices = [regex]::Replace($notices, '\[[^\]]+\]\(DEVELOPMENT.md#[^)]+\)', 'source repository docs/DEVELOPMENT.md')
[IO.File]::WriteAllText((Join-Path $stage 'docs/THIRD_PARTY.md'), $notices, $utf8)
$toolboxHelpPath = Join-Path $stage 'docs/features/TOOLBOX.md'
$toolboxHelp = [IO.File]::ReadAllText($toolboxHelpPath)
$toolboxHelp = [regex]::Replace($toolboxHelp, '(?m)^\[返回产品说明\].*$', '[返回使用手册](../USAGE.md)')
$toolboxHelp = [regex]::Replace($toolboxHelp, '(?s)本页是 GPIO 调试和网络测速的行为依据。最初方案及取舍见\s*\[ADR-0007\]\([^)]+\)，实际验证范围只记在 STATUS。', '本页说明 GPIO 调试和网络测速的操作效果、参数限制与处理方式。')
[IO.File]::WriteAllText($toolboxHelpPath, $toolboxHelp, $utf8)
$releaseNotePath = Join-Path $stage "docs/releases/$version.md"
$releaseNote = [IO.File]::ReadAllText($releaseNotePath).Replace(' · [验证与交付状态](../STATUS.md)', '')
[IO.File]::WriteAllText($releaseNotePath, $releaseNote, $utf8)
[void](New-Item -ItemType Directory -Path (Join-Path $stage 'licenses/MSVC') -Force)
$vcNotice = "MSVC x64 app-local runtime $vcVersion. Original Microsoft redistributable binaries from the Visual Studio Redist CRT directory.`n" +
    "Deployment: https://learn.microsoft.com/cpp/windows/redistributing-visual-cpp-files`n" +
    "Redistribution terms: https://visualstudio.microsoft.com/license-terms/`n"
[IO.File]::WriteAllText((Join-Path $stage 'licenses/MSVC/README.txt'), $vcNotice, $utf8)
$qtNotice = @'
This package dynamically links the unmodified Qt 6.11.2 libraries (Core, Gui,
Widgets, Network, Svg and the accompanying plugins) under LGPLv3.
Qt copyright: The Qt Company Ltd. and other contributors.
Corresponding upstream source: https://download.qt.io/archive/qt/6.11/6.11.2/single/
License texts and bundled-library notices are included in this directory.
Qt DLLs remain replaceable. The first-party LICENSE does not restrict replacing
or debugging modifications to these libraries as permitted by their licenses.
The upstream SDK and bundled tool sources are described in docs/THIRD_PARTY.md.
'@
[IO.File]::WriteAllText((Join-Path $stage 'licenses/Qt/README.txt'), $qtNotice, $utf8)
# The portable help must work without a source checkout.
$linkPattern = [regex]'\[[^\]]+\]\((?<target>[^)]+)\)'
foreach ($relative in @('README.md','docs/USAGE.md','docs/THIRD_PARTY.md','docs/features/TOOLBOX.md',"docs/releases/$version.md")) {
    $document = Get-Item -LiteralPath (Join-Path $stage $relative)
    foreach ($match in $linkPattern.Matches([IO.File]::ReadAllText($document.FullName))) {
        $target = $match.Groups['target'].Value.Trim().Trim('<', '>')
        if ($target.StartsWith('#') -or $target -match '^[a-zA-Z][a-zA-Z0-9+.-]*:') { continue }
        $pathPart = [Uri]::UnescapeDataString(($target -split '#', 2)[0])
        if ($pathPart -and -not (Test-Path -LiteralPath (Join-Path $document.DirectoryName $pathPart))) {
            throw "Broken package help link: $($document.Name) -> $target"
        }
    }
}
$files = @(Get-ChildItem -LiteralPath $stage -Recurse -File | Sort-Object FullName | ForEach-Object {
    [ordered]@{ path=$_.FullName.Substring($stage.Length + 1).Replace('\','/'); bytes=$_.Length;
               sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
})
$manifest = [ordered]@{ product='Oh My Debug'; version=$version; copyright=$copyrightOwner; developer=$developer;
    source_revision=$revision; source_dirty=$dirty; qt_version=$qtVersion;
    agent_version=$bundleManifest.agent_version; msvc_runtime_version=$vcVersion; msvc_runtime_deployment='app-local'; created_utc=[DateTime]::UtcNow.ToString('o'); files=$files }
[IO.File]::WriteAllText((Join-Path $stage 'manifest.json'), ($manifest | ConvertTo-Json -Depth 8), $utf8)
Compress-Archive -LiteralPath $stage -DestinationPath $archive -CompressionLevel Optimal
$hash = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
[IO.File]::WriteAllText("$archive.sha256", "$hash  $([IO.Path]::GetFileName($archive))`n", $utf8)
Write-Host "Created $archive ($($files.Count) payload files; source_dirty=$dirty)"
