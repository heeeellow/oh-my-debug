[CmdletBinding()]
param(
    [string]$RuntimeDirectory = '',
    [Parameter(Mandatory = $true)][string]$QtSourceDirectory,
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
$version = [IO.File]::ReadAllText((Join-Path $root 'VERSION.txt')).Trim()
if ($version -notmatch '^\d+\.\d+\.\d+$') { throw 'Invalid VERSION.txt' }
$exe = Join-Path $runtime 'OhMyDebug.exe'
$info = (Get-Item -LiteralPath $exe).VersionInfo
if ($info.ProductVersion -ne $version -or $info.ProductName -ne 'Oh My Debug' -or
    $info.OriginalFilename -ne 'OhMyDebug.exe' -or $info.IsDebug) {
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
                    'platforms/qwindows.dll','imageformats/qsvg.dll','iconengines/qsvgicon.dll','vc_redist.x64.exe')) {
    Copy-File (Join-Path $runtime $file) $file
}
foreach ($file in @('generic/qtuiotouchplugin.dll','imageformats/qgif.dll','imageformats/qico.dll',
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
foreach ($file in @('LICENSE','docs/USAGE.md','docs/THIRD_PARTY.md')) { Copy-File (Join-Path $root $file) $file }
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
$readme = "# Oh My Debug $version`n`nRun OhMyDebug.exe from this directory. Keep the complete package together.`n" +
    "If the MSVC runtime is missing, install vc_redist.x64.exe first.`n`n" +
    "See [usage](docs/USAGE.md), [first-party license](LICENSE) and [third-party notices](docs/THIRD_PARTY.md).`n" +
    "Copyright (c) 2026 Yanzhaohui. See manifest.json for build and file information.`n"
[IO.File]::WriteAllText((Join-Path $stage 'README.md'), $readme, $utf8)
$notices = [IO.File]::ReadAllText((Join-Path $stage 'docs/THIRD_PARTY.md'))
foreach ($dependency in @('asio','CSerialPort','tiny-process-library','libvterm','phosphor-icons','iperf3','nlohmann')) {
    $notices = $notices.Replace("../third_party/$dependency/", "../licenses/$dependency/")
}
$notices = $notices.Replace('../third_party/platform-tools/', '../platform-tools/').Replace('../third_party/scrcpy/', '../scrcpy/')
$notices = [regex]::Replace($notices, '\[[^\]]+\]\(DEVELOPMENT.md#[^)]+\)', 'source repository docs/DEVELOPMENT.md')
[IO.File]::WriteAllText((Join-Path $stage 'docs/THIRD_PARTY.md'), $notices, $utf8)
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
$files = @(Get-ChildItem -LiteralPath $stage -Recurse -File | Sort-Object FullName | ForEach-Object {
    [ordered]@{ path=$_.FullName.Substring($stage.Length + 1).Replace('\','/'); bytes=$_.Length;
               sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
})
$manifest = [ordered]@{ product='Oh My Debug'; version=$version; copyright='Yanzhaohui';
    source_revision=$revision; source_dirty=$dirty; qt_version=$qtVersion;
    agent_version=$bundleManifest.agent_version; created_utc=[DateTime]::UtcNow.ToString('o'); files=$files }
[IO.File]::WriteAllText((Join-Path $stage 'manifest.json'), ($manifest | ConvertTo-Json -Depth 8), $utf8)
Compress-Archive -LiteralPath $stage -DestinationPath $archive -CompressionLevel Optimal
$hash = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
[IO.File]::WriteAllText("$archive.sha256", "$hash  $([IO.Path]::GetFileName($archive))`n", $utf8)
Write-Host "Created $archive ($($files.Count) payload files; source_dirty=$dirty)"
