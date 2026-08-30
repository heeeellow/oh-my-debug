[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$requiredFiles = @(
    'AGENTS.md',
    'README.md',
    'docs/STATUS.md',
    'docs/PRODUCT.md',
    'docs/ARCHITECTURE.md',
    'docs/UI_DESIGN.md',
    'docs/DEVELOPMENT.md',
    'docs/AGENT_PROTOCOL_V1.md',
    'docs/features/SERIAL.md'
)

$errors = [System.Collections.Generic.List[string]]::new()

foreach ($relativePath in $requiredFiles) {
    $fullPath = Join-Path $repositoryRoot $relativePath
    if (-not (Test-Path -LiteralPath $fullPath -PathType Leaf)) {
        $errors.Add("Missing required document: $relativePath")
    }
}

$markdownFiles = @(
    Get-ChildItem -LiteralPath $repositoryRoot -File -Filter '*.md'
    Get-ChildItem -LiteralPath (Join-Path $repositoryRoot 'docs') -Recurse -File -Filter '*.md'
)

$linkPattern = [regex]'\[[^\]]+\]\((?<target>[^)]+)\)'

foreach ($markdownFile in $markdownFiles) {
    $content = Get-Content -LiteralPath $markdownFile.FullName -Raw
    foreach ($match in $linkPattern.Matches($content)) {
        $target = $match.Groups['target'].Value.Trim().Trim('<', '>')
        if ([string]::IsNullOrWhiteSpace($target) -or
            $target.StartsWith('#') -or
            $target -match '^[a-zA-Z][a-zA-Z0-9+.-]*:') {
            continue
        }

        $pathPart = ($target -split '#', 2)[0]
        $pathPart = [System.Uri]::UnescapeDataString($pathPart)
        if ([string]::IsNullOrWhiteSpace($pathPart)) {
            continue
        }

        $resolvedPath = [System.IO.Path]::GetFullPath(
            (Join-Path $markdownFile.DirectoryName $pathPart)
        )
        if (-not (Test-Path -LiteralPath $resolvedPath)) {
            $sourceRelative = [System.IO.Path]::GetRelativePath($repositoryRoot, $markdownFile.FullName)
            $errors.Add("Broken relative link: $sourceRelative -> $target")
        }
    }
}

if ($errors.Count -gt 0) {
    $errors | ForEach-Object { Write-Error $_ }
    exit 1
}

Write-Host ("Documentation check passed: {0} Markdown files; required entries and relative links are valid." -f $markdownFiles.Count)
