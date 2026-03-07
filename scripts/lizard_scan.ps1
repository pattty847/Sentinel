param(
    [string]$Target = "libs",
    [string]$Scope = "",
    [string]$Name = "",
    [string]$Language = "cpp",
    [int]$Top = 15,
    [int]$CcnThreshold = 15,
    [int]$NlocThreshold = 120,
    [int]$ParamThreshold = 6
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Convert-ToSlug {
    param([string]$Text)
    $slug = $Text.ToLowerInvariant()
    $slug = $slug -replace "[\\/]+", "-"
    $slug = $slug -replace "[^a-z0-9._-]", "-"
    $slug = $slug -replace "-+", "-"
    $slug = $slug.Trim("-.")
    if ([string]::IsNullOrWhiteSpace($slug)) {
        return "scope"
    }
    return $slug
}

function Get-RelativePath {
    param(
        [string]$BasePath,
        [string]$TargetPath
    )
    $baseFull = [System.IO.Path]::GetFullPath($BasePath)
    $targetFull = [System.IO.Path]::GetFullPath($TargetPath)
    $baseUri = New-Object System.Uri(($baseFull.TrimEnd('\') + '\'))
    $targetUri = New-Object System.Uri($targetFull)
    $relativeUri = $baseUri.MakeRelativeUri($targetUri)
    $relativePath = [System.Uri]::UnescapeDataString($relativeUri.ToString())
    return $relativePath -replace '/', '\'
}

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$pythonExe = Join-Path $PSScriptRoot ".venv/Scripts/python.exe"
if (-not (Test-Path $pythonExe)) {
    throw "Missing venv python at: $pythonExe"
}

$targetPath = $Target
if (-not [System.IO.Path]::IsPathRooted($targetPath)) {
    $targetPath = Join-Path $repoRoot $targetPath
}
$targetPath = (Resolve-Path $targetPath).Path

$relativeTarget = Get-RelativePath -BasePath $repoRoot -TargetPath $targetPath
$scopeSlug = if ([string]::IsNullOrWhiteSpace($Scope)) { Convert-ToSlug $relativeTarget } else { Convert-ToSlug $Scope }

if ([string]::IsNullOrWhiteSpace($Name)) {
    $timestamp = Get-Date -Format "yyyyMMdd-HHmmss"
    $nameSlug = "lizard-$($scopeSlug)-$timestamp"
} else {
    $nameSlug = Convert-ToSlug $Name
}

$outputDir = Join-Path $repoRoot "_agent/lizard/$scopeSlug"
New-Item -ItemType Directory -Path $outputDir -Force | Out-Null

$csvPath = Join-Path $outputDir "$nameSlug.csv"
$nsCsvPath = Join-Path $outputDir "$nameSlug.ns.csv"
$warningsPath = Join-Path $outputDir "$nameSlug.warnings.txt"
$summaryPath = Join-Path $outputDir "$nameSlug.summary.json"

& $pythonExe -m lizard $targetPath -l $Language --csv -o $csvPath
& $pythonExe -m lizard $targetPath -l $Language -E NS --csv -o $nsCsvPath

$warningOutput = & $pythonExe -m lizard $targetPath -l $Language -T "cyclomatic_complexity=$CcnThreshold" -T "nloc=$NlocThreshold" -T "parameter_count=$ParamThreshold" -w 2>&1
$warningOutput | Out-File -FilePath $warningsPath -Encoding ascii

$headers = @("nloc", "ccn", "token", "params", "length", "location", "file", "function", "signature", "start", "end")
$rows = Import-Csv -Path $csvPath -Header $headers | Where-Object {
    $_.file -and $_.function -and ($_.ccn -as [int]) -ne $null
}

$headersNs = @("nloc", "ccn", "token", "params", "length", "location", "file", "function", "signature", "start", "end", "nd")
$rowsNs = Import-Csv -Path $nsCsvPath -Header $headersNs | Where-Object {
    $_.file -and $_.function -and ($_.nd -as [int]) -ne $null
}

$fileAgg = $rows | Group-Object file | ForEach-Object {
    $groupRows = @($_.Group)
    $ccnValues = $groupRows | ForEach-Object { [int]$_.ccn }
    $nlocValues = $groupRows | ForEach-Object { [int]$_.nloc }
    [pscustomobject]@{
        file = $_.Name
        functions = $groupRows.Count
        avg_ccn = [math]::Round((($ccnValues | Measure-Object -Average).Average), 2)
        max_ccn = ($ccnValues | Measure-Object -Maximum).Maximum
        total_nloc = ($nlocValues | Measure-Object -Sum).Sum
        high_ccn_funcs = @($ccnValues | Where-Object { $_ -ge $CcnThreshold }).Count
    }
}

$summary = [pscustomobject]@{
    meta = [pscustomobject]@{
        target = $relativeTarget
        target_abs = $targetPath
        language = $Language
        scope = $scopeSlug
        name = $nameSlug
        generated_at = (Get-Date).ToString("s")
        thresholds = [pscustomobject]@{
            ccn = $CcnThreshold
            nloc = $NlocThreshold
            parameter_count = $ParamThreshold
        }
    }
    counts = [pscustomobject]@{
        total_functions = @($rows).Count
        over_ccn_10 = @($rows | Where-Object { [int]$_.ccn -gt 10 }).Count
        over_ccn_15 = @($rows | Where-Object { [int]$_.ccn -gt 15 }).Count
        over_ccn_25 = @($rows | Where-Object { [int]$_.ccn -gt 25 }).Count
        over_nloc_100 = @($rows | Where-Object { [int]$_.nloc -gt 100 }).Count
        over_param_5 = @($rows | Where-Object { [int]$_.params -gt 5 }).Count
    }
    top_ccn = $rows | Sort-Object { [int]$_.ccn } -Descending | Select-Object -First $Top file, function, ccn, nloc, params, start, end
    top_nloc = $rows | Sort-Object { [int]$_.nloc } -Descending | Select-Object -First $Top file, function, ccn, nloc, params, start, end
    top_params = $rows | Sort-Object { [int]$_.params } -Descending | Select-Object -First $Top file, function, ccn, nloc, params, start, end
    top_nested = $rowsNs | Sort-Object -Property @{ Expression = { [int]$_.nd }; Descending = $true }, @{ Expression = { [int]$_.ccn }; Descending = $true } | Select-Object -First $Top file, function, nd, ccn, nloc, params, start, end
    worst_files = $fileAgg | Sort-Object high_ccn_funcs, max_ccn, avg_ccn -Descending | Select-Object -First $Top file, functions, avg_ccn, max_ccn, total_nloc, high_ccn_funcs
}

$summary | ConvertTo-Json -Depth 8 | Out-File -FilePath $summaryPath -Encoding ascii

Write-Host "lizard target: $relativeTarget"
Write-Host "output dir: $outputDir"
Write-Host "csv: $csvPath"
Write-Host "ns csv: $nsCsvPath"
Write-Host "warnings: $warningsPath"
Write-Host "summary: $summaryPath"
