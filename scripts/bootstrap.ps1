<#
.SYNOPSIS
  Prepare a collaborator machine for building AudioSub.

.DESCRIPTION
  This script checks the local environment, optionally installs the prebuilt
  WebRTC SDK, and then invokes scripts/build.ps1.

  Typical collaborator flow:
    .\scripts\bootstrap.ps1 -Url <release-zip-url>

  If the SDK is already present:
    .\scripts\bootstrap.ps1
#>

param(
    [string]$Url = "",
    [string]$LocalSdkPath = "",
    [switch]$ForceSdk,
    [string]$Config = "Release",
    [switch]$Clean,
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"

$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$FetchScript = Join-Path $PSScriptRoot "fetch-webrtc-sdk.ps1"
$BuildScript = Join-Path $PSScriptRoot "build.ps1"
$SdkLib = Join-Path $RepoRoot "third_party/webrtc-sdk/lib/webrtc.lib"
$VersionPath = Join-Path $RepoRoot "third_party/webrtc-sdk/VERSION.txt"

function Get-VsWherePath {
    $candidates = @(
        (Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"),
        (Join-Path $env:ProgramFiles "Microsoft Visual Studio\Installer\vswhere.exe")
    ) | Where-Object { $_ -and (Test-Path $_) }

    return $candidates | Select-Object -First 1
}

function Get-VisualStudioInfo {
    $vswhere = Get-VsWherePath
    if (-not $vswhere) {
        return $null
    }

    $installationPath = & $vswhere -latest -products * `
        -requires Microsoft.Component.MSBuild `
        -property installationPath
    if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($installationPath)) {
        return $null
    }

    [PSCustomObject]@{
        InstallationPath = $installationPath.Trim()
    }
}

function Resolve-CommandPath {
    param([string]$Name)

    $cmd = Get-Command $Name -ErrorAction SilentlyContinue
    if ($cmd) {
        return $cmd.Source
    }
    return $null
}

function Test-PythonReady {
    $python = Resolve-CommandPath -Name "python"
    if (-not $python) {
        Write-Host "Python not found in PATH." -ForegroundColor Yellow
        Write-Host "  Needed for signaling server and test scripts." -ForegroundColor Yellow
        return $false
    }

    Write-Host "Python        : $python" -ForegroundColor DarkCyan
    return $true
}

function Test-BuildToolsReady {
    $vsInfo = Get-VisualStudioInfo
    if ($vsInfo) {
        Write-Host "Visual Studio : $($vsInfo.InstallationPath)" -ForegroundColor DarkCyan
        return $true
    }

    Write-Host "Visual Studio Build Tools not found via vswhere." -ForegroundColor Yellow
    Write-Host "  Needed for C++ compilation." -ForegroundColor Yellow
    return $false
}

function Show-SdkInfo {
    if ((Test-Path $SdkLib) -and (Test-Path $VersionPath)) {
        Write-Host "WebRTC SDK    : ready" -ForegroundColor Green
        Get-Content $VersionPath | ForEach-Object { Write-Host "  $_" }
    } else {
        Write-Host "WebRTC SDK    : missing" -ForegroundColor Yellow
    }
}

Write-Host "===== bootstrap =====" -ForegroundColor Cyan
Write-Host "Repo          : $RepoRoot"

$pythonReady = Test-PythonReady
$buildToolsReady = Test-BuildToolsReady
Show-SdkInfo

if (-not (Test-Path $FetchScript)) {
    throw "Missing script: $FetchScript"
}
if (-not (Test-Path $BuildScript)) {
    throw "Missing script: $BuildScript"
}

$needSdkInstall = $ForceSdk -or (-not (Test-Path $SdkLib))
if ($needSdkInstall) {
    Write-Host ""
    Write-Host "===== prepare WebRTC SDK =====" -ForegroundColor Cyan

    if (-not [string]::IsNullOrWhiteSpace($LocalSdkPath)) {
        & $FetchScript -LocalPath $LocalSdkPath -Force:$ForceSdk
        if ($LASTEXITCODE -ne 0) { throw "fetch-webrtc-sdk.ps1 failed" }
    } elseif (-not [string]::IsNullOrWhiteSpace($Url)) {
        & $FetchScript -Url $Url -Force:$ForceSdk
        if ($LASTEXITCODE -ne 0) { throw "fetch-webrtc-sdk.ps1 failed" }
    } else {
        & $FetchScript -Force:$ForceSdk
        if ($LASTEXITCODE -ne 0) { throw "fetch-webrtc-sdk.ps1 failed" }
    }
}

Write-Host ""
Write-Host "===== environment summary =====" -ForegroundColor Cyan
Write-Host ("Python        : {0}" -f ($(if ($pythonReady) { "OK" } else { "MISSING" })))
Write-Host ("Build Tools   : {0}" -f ($(if ($buildToolsReady) { "OK" } else { "MISSING" })))
Write-Host ("WebRTC SDK    : {0}" -f ($(if (Test-Path $SdkLib) { "OK" } else { "MISSING" })))

if (-not $buildToolsReady) {
    Write-Host ""
    Write-Host "Install Visual Studio 2022 Build Tools with C++ workload, then rerun this script." -ForegroundColor Red
    exit 3
}

if (-not (Test-Path $SdkLib)) {
    Write-Host ""
    Write-Host "WebRTC SDK is still missing. Provide -Url or -LocalSdkPath, or configure DefaultReleaseUrl in fetch-webrtc-sdk.ps1." -ForegroundColor Red
    exit 4
}

if ($SkipBuild) {
    Write-Host ""
    Write-Host "SkipBuild enabled. Environment preparation finished." -ForegroundColor Green
    exit 0
}

Write-Host ""
Write-Host "===== build =====" -ForegroundColor Cyan
& $BuildScript -Config $Config -Clean:$Clean
exit $LASTEXITCODE
