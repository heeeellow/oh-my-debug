param(
    [switch]$Build,
    [ValidateSet('Debug', 'Release', 'All')]
    [string]$Configuration = 'Debug',
    [switch]$RunSelfTest
)

$ErrorActionPreference = 'Stop'
$repositoryRoot = Split-Path -Parent $PSScriptRoot
Push-Location $repositoryRoot
try {
    & (Join-Path $PSScriptRoot 'check-project-docs.ps1')

    & git diff --check
    if ($LASTEXITCODE -ne 0) {
        throw 'git diff --check failed'
    }

    $sourceFiles = Get-ChildItem 'src\core', 'src\application', 'src\adapters', 'src\agent' `
        -Recurse -File -Include '*.h', '*.cpp' |
        Where-Object { $_.FullName -notmatch '\\(out|obj|x64)\\' }
    $qtMatches = $sourceFiles | Select-String -Pattern `
        '#include\s*[<"]Qt|\bQString\b|\bQByteArray\b|\bQObject\b'
    if ($qtMatches) {
        $qtMatches | ForEach-Object { Write-Error $_.ToString() }
        throw 'Qt dependency escaped src/ui'
    }

    $uiFiles = Get-ChildItem 'src\ui' -Recurse -File -Include '*.h', '*.cpp'
    $adapterMatches = $uiFiles | Select-String -Pattern '#include\s*[<"]adapters[\\/]'
    if ($adapterMatches) {
        $adapterMatches | ForEach-Object { Write-Error $_.ToString() }
        throw 'UI directly includes a concrete adapter'
    }

    $trackedGenerated = & git ls-files -- 'build/**' 'src/agent/out/**' `
        'src/agent/obj/**' 'src/agent/x64/**'
    if ($trackedGenerated) {
        throw "Generated files are tracked:`n$($trackedGenerated -join "`n")"
    }

    $projectText = Get-Content -Raw 'TestTools.vcxproj'
    $missingProjectEntries = @()
    Get-ChildItem 'src\application', 'src\core', 'src\adapters' -Recurse -File |
        Where-Object { $_.Extension -in @('.h', '.cpp') } |
        ForEach-Object {
            $relative = $_.FullName.Substring($repositoryRoot.Length + 1).Replace('/', '\')
            if (-not $projectText.Contains($relative)) {
                $missingProjectEntries += $relative
            }
        }
    if ($missingProjectEntries.Count -gt 0) {
        throw "First-party source missing from TestTools.vcxproj:`n$($missingProjectEntries -join "`n")"
    }

    Write-Host 'Static baseline checks passed.'

    if ($Build) {
        $msBuild = (Get-Command msbuild.exe -ErrorAction SilentlyContinue).Source
        if (-not $msBuild) {
            $vswhere = Join-Path ${env:ProgramFiles(x86)} `
                'Microsoft Visual Studio\Installer\vswhere.exe'
            if (Test-Path -LiteralPath $vswhere) {
                $msBuild = & $vswhere -latest -products * `
                    -requires Microsoft.Component.MSBuild `
                    -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
            }
        }
        if (-not $msBuild) {
            throw 'MSBuild was not found. Run from Developer PowerShell or install Visual Studio Build Tools.'
        }
        $qtMsBuild = Join-Path $env:LOCALAPPDATA 'QtMsBuild'
        $configurations = if ($Configuration -eq 'All') {
            @('Debug', 'Release')
        } else {
            @($Configuration)
        }
        foreach ($item in $configurations) {
            & $msBuild '.\TestTools.vcxproj' /m /nologo `
                "/p:Configuration=$item" '/p:Platform=x64' "/p:QtMsBuild=$qtMsBuild"
            if ($LASTEXITCODE -ne 0) {
                throw "TestTools $item build failed"
            }

            $windowsOutput = Join-Path $repositoryRoot "build\windows\x64\$item"
            $qtSuffix = if ($item -eq 'Debug') { 'd' } else { '' }
            $requiredRuntimeFiles = @(
                'TestTools.exe',
                "Qt6Core$qtSuffix.dll",
                "Qt6Gui$qtSuffix.dll",
                "Qt6Widgets$qtSuffix.dll",
                "platforms\qwindows$qtSuffix.dll"
            )
            $missingRuntimeFiles = $requiredRuntimeFiles |
                Where-Object { -not (Test-Path -LiteralPath (Join-Path $windowsOutput $_)) }
            if ($missingRuntimeFiles) {
                throw "Qt runtime deployment incomplete for $item`: $($missingRuntimeFiles -join ', ')"
            }
        }

        $cmake = (Get-Command cmake.exe -ErrorAction SilentlyContinue).Source
        if (-not $cmake) {
            throw 'CMake was not found for host-side Agent tests. Add it to PATH or set TESTTOOLS_CMAKE_PROGRAM.'
        }
        $coreTestBuild = Join-Path $repositoryRoot 'build\tests\host'
        & $cmake -S 'tests' -B $coreTestBuild
        if ($LASTEXITCODE -ne 0) { throw 'Core/application host test configure failed' }
        & $cmake --build $coreTestBuild --config Release --parallel
        if ($LASTEXITCODE -ne 0) { throw 'Core/application host test build failed' }

        $ctest = Join-Path (Split-Path -Parent $cmake) 'ctest.exe'
        if (-not (Test-Path -LiteralPath $ctest -PathType Leaf)) {
            $ctest = (Get-Command ctest.exe -ErrorAction SilentlyContinue).Source
        }
        if (-not $ctest) { throw 'CTest was not found for host tests.' }
        & $ctest --test-dir $coreTestBuild -C Release --output-on-failure
        if ($LASTEXITCODE -ne 0) { throw 'Core/application host contract tests failed' }

        $hostTestBuild = Join-Path $repositoryRoot 'build\agent\host-tests'
        & $cmake -S 'src\agent' -B $hostTestBuild '-DTESTTOOLS_BUILD_TESTS=ON'
        if ($LASTEXITCODE -ne 0) { throw 'Agent host test configure failed' }
        & $cmake --build $hostTestBuild --config Release --parallel
        if ($LASTEXITCODE -ne 0) { throw 'Agent host test build failed' }
        & $ctest --test-dir $hostTestBuild -C Release --output-on-failure
        if ($LASTEXITCODE -ne 0) { throw 'Agent host contract tests failed' }
    }

    if ($RunSelfTest) {
        $selfTestConfiguration = if ($Configuration -eq 'Release') { 'Release' } else { 'Debug' }
        $executable = Join-Path $repositoryRoot `
            "build\windows\x64\$selfTestConfiguration\TestTools.exe"
        if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
            throw "Self-test executable not found: $executable"
        }
        $qtInstallName = [regex]::Match(
            (Get-Content -Raw 'TestTools.vcxproj'), '<QtInstall>([^<]+)</QtInstall>').Groups[1].Value
        if ($qtInstallName) {
            $qtRegistry = "HKCU:\Software\QtProject\QtVsTools\Versions\$qtInstallName"
            $qtInstallDirectory = (Get-ItemProperty -Path $qtRegistry -ErrorAction SilentlyContinue).InstallDir
            if ($qtInstallDirectory) {
                $env:PATH = (Join-Path $qtInstallDirectory 'bin') + ';' + $env:PATH
            }
        }
        $selfTest = Start-Process -FilePath $executable -ArgumentList '--self-test' `
            -Wait -PassThru
        if ($selfTest.ExitCode -ne 0) { throw 'TestTools self-test failed' }
    }
} finally {
    Pop-Location
}
