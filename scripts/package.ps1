<#
.SYNOPSIS
    Packages a Release build of Takeoff into a redistributable zip with SHA256 checksum.

.DESCRIPTION
    Builds the Release x64 configuration using CMake (or MSBuild), creates a staging
    directory with Takeoff.exe, README.md, and LICENSE, and outputs a zip archive and
    checksum into the 'dist' directory.
#>

param (
    [string]$Version = "1.0.0",
    [switch]$SkipBuild = $false
)

$ErrorActionPreference = "Stop"
$rootDir = (Get-Item $PSScriptRoot).Parent.FullName
Set-Location $rootDir

Write-Host "==> Packaging Takeoff v$Version..." -ForegroundColor Cyan

if (-not $SkipBuild) {
    Write-Host "==> Building Release x64 with CMake..." -ForegroundColor Yellow
    if (-not (Test-Path "build")) {
        cmake -S . -B build -A x64
    }
    cmake --build build --config Release --target Takeoff
}

$exePath = Join-Path $rootDir "build\Release\Takeoff.exe"
if (-not (Test-Path $exePath)) {
    # Fallback to Visual Studio output path if built with Takeoff.sln
    $exePath = Join-Path $rootDir "x64\Release\Takeoff.exe"
}

if (-not (Test-Path $exePath)) {
    Write-Error "Could not find Takeoff.exe in either build\Release\ or x64\Release\. Build failed?"
    exit 1
}

$distDir = Join-Path $rootDir "dist"
$stageDir = Join-Path $distDir "staging"
$zipName = "Takeoff-v$Version-windows-x64.zip"
$zipPath = Join-Path $distDir $zipName

if (Test-Path $stageDir) { Remove-Item -Recurse -Force $stageDir }
if (Test-Path $zipPath) { Remove-Item -Force $zipPath }

New-Item -ItemType Directory -Force -Path $stageDir | Out-Null

Copy-Item $exePath -Destination $stageDir\
Copy-Item (Join-Path $rootDir "README.md") -Destination $stageDir\
Copy-Item (Join-Path $rootDir "LICENSE") -Destination $stageDir\

Write-Host "==> Creating archive: $zipName..." -ForegroundColor Yellow
Compress-Archive -Path "$stageDir\*" -DestinationPath $zipPath -Force

$sha256 = (Get-FileHash -Path $zipPath -Algorithm SHA256).Hash
$shaFile = "$zipPath.sha256"
"$sha256  $zipName" | Out-File -FilePath $shaFile -Encoding ascii

Remove-Item -Recurse -Force $stageDir

Write-Host ""
Write-Host "Packaging complete!" -ForegroundColor Green
Write-Host "  Archive:  $zipPath"
Write-Host "  SHA256:   $sha256"
Write-Host "  Checksum: $shaFile"
