<#
    windows-package-release.ps1
    Package a Windows release into a portable folder (default: Desktop\Sentinel).

    Usage:
      powershell -ExecutionPolicy Bypass -File scripts/release/windows-package-release.ps1
      powershell -ExecutionPolicy Bypass -File scripts/release/windows-package-release.ps1 -Build
      powershell -ExecutionPolicy Bypass -File scripts/release/windows-package-release.ps1 -TargetDir "C:\Releases\Sentinel"
#>

param(
    [string]$TargetDir = "$HOME\Desktop\Sentinel",
    [string]$BuildDir = "",
    [ValidateSet("Release", "RelWithDebInfo", "Debug")]
    [string]$Config = "Release",
    [switch]$Build
)

$ErrorActionPreference = "Stop"

function Fail([string]$Message) {
    Write-Error $Message
    exit 1
}

function Copy-DirContents([string]$SourceDir, [string]$DestDir) {
    if (-not (Test-Path $SourceDir)) {
        Fail "Missing directory: $SourceDir"
    }
    New-Item -ItemType Directory -Path $DestDir -Force | Out-Null
    Copy-Item -Path (Join-Path $SourceDir "*") -Destination $DestDir -Recurse -Force
}

function Resolve-Windeployqt {
    if ($env:QT_MSVC) {
        $candidate = Join-Path $env:QT_MSVC "bin\windeployqt.exe"
        if (Test-Path $candidate) {
            return $candidate
        }
    }

    $cmd = Get-Command windeployqt.exe -ErrorAction SilentlyContinue
    if ($cmd) {
        return $cmd.Source
    }

    return $null
}

function Resolve-OpenSslExe {
    $cmd = Get-Command openssl.exe -ErrorAction SilentlyContinue
    if ($cmd) {
        return $cmd.Source
    }

    $candidates = @(
        "$env:ProgramFiles\Git\usr\bin\openssl.exe",
        "$env:ProgramFiles\Git\mingw64\bin\openssl.exe",
        "$env:ProgramFiles(x86)\Git\usr\bin\openssl.exe",
        "$env:ProgramFiles(x86)\Git\mingw64\bin\openssl.exe"
    )

    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path $candidate)) {
            return $candidate
        }
    }

    return $null
}

function Resolve-CaBundlePath([string]$RepoRootPath) {
    $repoBundle = Join-Path $RepoRootPath "resources\certs\ca-bundle.crt"
    if (Test-Path $repoBundle) {
        return $repoBundle
    }

    $candidates = @(
        "$env:ProgramFiles\Git\mingw64\ssl\certs\ca-bundle.crt",
        "$env:ProgramFiles\Git\mingw64\etc\ssl\certs\ca-bundle.crt",
        "$env:ProgramFiles\Git\usr\ssl\cert.pem",
        "$env:ProgramFiles\Git\usr\ssl\certs\ca-bundle.crt"
    )

    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path $candidate)) {
            return $candidate
        }
    }

    return $null
}

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Resolve-Path (Join-Path $scriptDir "..\..")
if ([string]::IsNullOrWhiteSpace($BuildDir)) {
    $BuildDir = Join-Path $repoRoot "build\windows-msvc-vs"
}

$guiBuildDir = Join-Path $BuildDir "apps\sentinel-gui\$Config"
$serverBuildDir = Join-Path $BuildDir "apps\sentinel-server\$Config"
$guiExe = Join-Path $guiBuildDir "sentinel-gui.exe"
$serverExe = Join-Path $serverBuildDir "sentinel-server.exe"

Write-Host "Repo root:   $repoRoot"
Write-Host "Build dir:   $BuildDir"
Write-Host "Config:      $Config"
Write-Host "Target dir:  $TargetDir"

if ($Build) {
    Write-Host "`n[1/5] Configuring + building release..."
    Push-Location $repoRoot
    try {
        & cmake --preset windows-msvc-vs
        if ($LASTEXITCODE -ne 0) {
            Fail "cmake configure failed."
        }

        & cmake --build --preset windows-msvc-vs --config $Config
        if ($LASTEXITCODE -ne 0) {
            Fail "cmake build failed."
        }
    } finally {
        Pop-Location
    }
}

if (-not (Test-Path $guiExe)) {
    Fail "Missing $guiExe. Build first: cmake --preset windows-msvc-vs && cmake --build --preset windows-msvc-vs --config $Config"
}
if (-not (Test-Path $serverExe)) {
    Fail "Missing $serverExe. Build first: cmake --preset windows-msvc-vs && cmake --build --preset windows-msvc-vs --config $Config"
}

Write-Host "`n[2/5] Creating package folder..."
if (Test-Path $TargetDir) {
    Remove-Item -Path $TargetDir -Recurse -Force
}
New-Item -ItemType Directory -Path $TargetDir -Force | Out-Null

Write-Host "[3/5] Copying app binaries and runtime files..."
Copy-DirContents -SourceDir $guiBuildDir -DestDir $TargetDir
Copy-DirContents -SourceDir $serverBuildDir -DestDir $TargetDir

Write-Host "[4/5] Copying config, certs, and QML assets..."
Copy-DirContents -SourceDir (Join-Path $repoRoot "config") -DestDir (Join-Path $TargetDir "config")
Copy-DirContents -SourceDir (Join-Path $repoRoot "certs") -DestDir (Join-Path $TargetDir "certs")

$caBundleSource = Resolve-CaBundlePath -RepoRootPath $repoRoot
if ($caBundleSource) {
    $caBundleDestDir = Join-Path $TargetDir "resources\certs"
    New-Item -ItemType Directory -Path $caBundleDestDir -Force | Out-Null
    Copy-Item -Path $caBundleSource -Destination (Join-Path $caBundleDestDir "ca-bundle.crt") -Force
} else {
    Fail "No CA bundle found. Expected resources\certs\ca-bundle.crt in repo or Git for Windows CA bundle."
}

$qmlTarget = Join-Path $TargetDir "libs\gui\qml"
Copy-DirContents -SourceDir (Join-Path $repoRoot "libs\gui\qml") -DestDir $qmlTarget

$qmlDirFile = Join-Path $repoRoot "libs\gui\qmldir"
if (Test-Path $qmlDirFile) {
    New-Item -ItemType Directory -Path (Join-Path $TargetDir "libs\gui") -Force | Out-Null
    Copy-Item -Path $qmlDirFile -Destination (Join-Path $TargetDir "libs\gui\qmldir") -Force
}

$repoReadme = Join-Path $repoRoot "README.md"
if (Test-Path $repoReadme) {
    Copy-Item -Path $repoReadme -Destination (Join-Path $TargetDir "README.md") -Force
}

$launchReadme = Join-Path $repoRoot "scripts\release\LAUNCH_README.md"
if (Test-Path $launchReadme) {
    Copy-Item -Path $launchReadme -Destination (Join-Path $TargetDir "LAUNCH_README.md") -Force
}

$pkgCert = Join-Path $TargetDir "certs\sentinel-server.crt"
$pkgKey = Join-Path $TargetDir "certs\sentinel-server.key"
if (-not ((Test-Path $pkgCert) -and (Test-Path $pkgKey))) {
    Write-Host "Generating package TLS certs (sentinel-server.crt/key)..."
    Push-Location (Join-Path $TargetDir "certs")
    try {
        $opensslExe = Resolve-OpenSslExe
        if (-not $opensslExe) {
            Fail "OpenSSL is required to generate certs. Install Git for Windows or add openssl to PATH."
        }
        $opensslDir = Split-Path -Parent $opensslExe
        if ($env:Path -notlike "*$opensslDir*") {
            $env:Path = "$opensslDir;$env:Path"
        }
        & powershell -NoProfile -ExecutionPolicy Bypass -File ".\gen-certs.ps1"
        if ($LASTEXITCODE -ne 0) {
            Fail "Failed to generate TLS certs in package."
        }
    } finally {
        Pop-Location
    }
}

$windeployqt = Resolve-Windeployqt
if ($windeployqt) {
    Write-Host "[5/5] Running windeployqt to ensure bundled deps..."
    $guiTargetExe = Join-Path $TargetDir "sentinel-gui.exe"
    $serverTargetExe = Join-Path $TargetDir "sentinel-server.exe"
    $qmlDir = Join-Path $repoRoot "libs\gui\qml"

    & $windeployqt --release --qmldir $qmlDir $guiTargetExe
    if ($LASTEXITCODE -ne 0) {
        Fail "windeployqt failed for sentinel-gui.exe"
    }

    & $windeployqt --release --no-opengl --no-system-d3d-compiler --no-translations $serverTargetExe
    if ($LASTEXITCODE -ne 0) {
        Fail "windeployqt failed for sentinel-server.exe"
    }
} else {
    Write-Warning "windeployqt.exe not found (set QT_MSVC or PATH). Package may miss Qt DLLs/plugins."
}

$runPs1 = @'
param()
$ErrorActionPreference = "Stop"
Set-Location (Split-Path -Parent $MyInvocation.MyCommand.Path)

$listening = Get-NetTCPConnection -State Listen -LocalPort 8080 -ErrorAction SilentlyContinue
if ($listening) {
    Write-Host "Port 8080 is in use. Stop existing sentinel-server and run again."
    exit 1
}

$requiredFiles = @(
    "certs\sentinel-server.crt",
    "certs\sentinel-server.key",
    "resources\certs\ca-bundle.crt",
    "config\server_config.yaml",
    "config\client_config.yaml"
)
foreach ($f in $requiredFiles) {
    if (-not (Test-Path $f)) {
        Write-Host "Missing required file: $f"
        exit 1
    }
}

$env:SENTINEL_QML_PATH = Join-Path (Get-Location) "libs\gui\qml\DepthChartView.qml"
Write-Host "Starting sentinel-server..."
$server = Start-Process -FilePath ".\sentinel-server.exe" -PassThru
Start-Sleep -Seconds 1
if ($server.HasExited) {
    Write-Host "sentinel-server exited immediately. Check certs and resources\certs\ca-bundle.crt, then run again."
    exit 1
}

try {
    Write-Host "Starting sentinel-gui..."
    Start-Process -FilePath ".\sentinel-gui.exe" -Wait
} finally {
    if ($server -and -not $server.HasExited) {
        Stop-Process -Id $server.Id -Force
    }
}
'@
Set-Content -Path (Join-Path $TargetDir "run.ps1") -Value $runPs1 -Encoding ASCII

$runCmd = @'
@echo off
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0run.ps1"
endlocal
'@
Set-Content -Path (Join-Path $TargetDir "run.cmd") -Value $runCmd -Encoding ASCII

Write-Host ""
Write-Host "Done. Portable release created at:"
Write-Host "  $TargetDir"
Write-Host ""
Write-Host "Run from package folder:"
Write-Host "  .\run.cmd"
Write-Host "or"
Write-Host "  powershell -ExecutionPolicy Bypass -File .\run.ps1"
