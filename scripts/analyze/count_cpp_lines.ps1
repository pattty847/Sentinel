# Count lines of code and estimate LLM tokens in all .cpp, .h, and .hpp files under libs/

# count_cpp_lines.ps1 - Count lines of code and estimate LLM tokens in all .cpp, .h, and .hpp files under libs/ on Windows PowerShell

param(
    [ValidateSet("lines", "tokens", "name")]
    [string]$By = "lines",
    
    [switch]$Asc,
    
    [switch]$FullPath,
    
    [switch]$Csv,
    
    [switch]$Md,
    
    [string]$Ext = "cpp,h,hpp,qml",
    
    [switch]$Help
)

$ErrorActionPreference = "Stop"

function Show-Usage {
    $scriptName = if ($PSCommandPath) { Split-Path -Leaf $PSCommandPath } else { "count_cpp_lines.ps1" }
    Write-Host @"
Usage: $scriptName [options]

Options:
  --By <lines|tokens|name>   Sort by column (default: lines)
  --Asc                      Sort ascending (default: descending)
  --FullPath                 Show paths relative to libs/ instead of basenames
  --Csv                      Output CSV (no header for easy piping)
  --Md                       Output Markdown table
  --Ext <csv>                Only include these extensions (comma-separated). Default: cpp,h,hpp,qml
  -Help                      Show this help and exit
"@
}

if ($Help) {
    Show-Usage
    exit 0
}

# Get script directory - works across PowerShell versions
if ($PSScriptRoot) {
    $ScriptDir = $PSScriptRoot
} elseif ($MyInvocation.MyCommand.Path) {
    $ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
} else {
    # Last resort: use current location (less reliable)
    $ScriptDir = $PWD.Path
    Write-Warning "Could not determine script directory, using current directory: $ScriptDir"
}

# Script is in scripts/analyze/, so go up two levels to repo root
$RepoRoot = Split-Path -Parent (Split-Path -Parent $ScriptDir)
$LibsDir = Join-Path $RepoRoot "libs"

if (-not (Test-Path $LibsDir)) {
    Write-Error "libs/ directory not found at: $LibsDir"
    exit 1
}

# Function to estimate tokens (rough approximation: 4 characters per token)
function Estimate-Tokens {
    param([string]$FilePath)
    $charCount = (Get-Content $FilePath -Raw -ErrorAction SilentlyContinue).Length
    return [math]::Floor($charCount / 4)
}

# Parse extensions
$extensions = $Ext -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ -ne '' }

# Build file list
$files = @()
foreach ($ext in $extensions) {
    $pattern = "*.$ext"
    $found = Get-ChildItem -Path $LibsDir -Filter $pattern -Recurse -File -ErrorAction SilentlyContinue
    $files += $found
}

# Process files
$rows = @()
$totalLines = 0
$totalTokens = 0

foreach ($file in $files) {
    $lineCount = (Get-Content $file.FullName -ErrorAction SilentlyContinue | Measure-Object -Line).Lines
    $tokenCount = Estimate-Tokens -FilePath $file.FullName
    
    $totalLines += $lineCount
    $totalTokens += $tokenCount
    
    if ($FullPath) {
        $displayName = $file.FullName.Replace($LibsDir + '\', '').Replace('\', '/')
    } else {
        $displayName = $file.Name
    }
    
    $rows += [PSCustomObject]@{
        File = $displayName
        Lines = $lineCount
        Tokens = $tokenCount
    }
}

# Sort
$sortedRows = switch ($By) {
    "lines" { 
        if ($Asc) { $rows | Sort-Object Lines }
        else { $rows | Sort-Object Lines -Descending }
    }
    "tokens" { 
        if ($Asc) { $rows | Sort-Object Tokens }
        else { $rows | Sort-Object Tokens -Descending }
    }
    "name" { 
        if ($Asc) { $rows | Sort-Object File }
        else { $rows | Sort-Object File -Descending }
    }
}

# Output
if ($Csv) {
    Write-Host "file,lines,tokens"
    foreach ($row in $sortedRows) {
        Write-Host "$($row.File),$($row.Lines),$($row.Tokens)"
    }
    Write-Host "TOTAL,$totalLines,$totalTokens"
    exit 0
}

if ($Md) {
    Write-Host "| File | Lines | Tokens |"
    Write-Host "|:-----|------:|-------:|"
    foreach ($row in $sortedRows) {
        Write-Host "| $($row.File) | $($row.Lines) | $($row.Tokens) |"
    }
    Write-Host "| TOTAL | $totalLines | $totalTokens |"
    exit 0
}

# Default pretty table
Write-Host ("{0,-50} {1,10} {2,10}" -f "File", "Lines", "Tokens")
Write-Host ("{0,-50} {1,10} {2,10}" -f ("-" * 50), ("-" * 10), ("-" * 10))
foreach ($row in $sortedRows) {
    Write-Host ("{0,-50} {1,10} {2,10}" -f $row.File, $row.Lines, $row.Tokens)
}
Write-Host ("{0,-50} {1,10} {2,10}" -f "TOTAL", $totalLines, $totalTokens)
Write-Host ""
Write-Host "Token estimation uses ~4 characters per token approximation"

