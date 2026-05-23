<#
.SYNOPSIS
  Download or link the prebuilt WebRTC SDK for AudioSub.

.DESCRIPTION
  Populates third_party/webrtc-sdk/ with:
    lib/webrtc.lib
    include/
    VERSION.txt

  Modes:
    1) Download from URL (GitHub Release zip)     -Url <url>
    2) Link/copy from local SDK directory         -LocalPath <dir>
    3) Default URL from script or env var

.EXAMPLE
  .\scripts\fetch-webrtc-sdk.ps1
  .\scripts\fetch-webrtc-sdk.ps1 -Url "https://github.com/you/AudioSub/releases/download/webrtc-sdk-v1.0.0/webrtc-sdk-win-x64-release.zip"
  .\scripts\fetch-webrtc-sdk.ps1 -LocalPath E:\Cpp\Codes\AudioSub\dist\webrtc-sdk-win-x64-release-771e6489
  $env:AUDIOSUB_WEBRTC_SDK_URL="https://github.com/you/AudioSub/releases/download/webrtc-sdk-v1.0.0/webrtc-sdk-win-x64-release.zip"
  .\scripts\fetch-webrtc-sdk.ps1
#>

param(
    [string]$Url = "",
    [string]$LocalPath = "",
    [switch]$Force
)

$ErrorActionPreference = "Stop"

$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$SdkDir = Join-Path $RepoRoot "third_party/webrtc-sdk"
$LibPath = Join-Path $SdkDir "lib/webrtc.lib"
$VersionPath = Join-Path $SdkDir "VERSION.txt"

# =====================================================================
# TEAM SETUP: configure ONE stable SDK download source for collaborators
# =====================================================================
# Recommended:
#   After uploading the zip to GitHub Releases, paste the direct asset URL into
#   $DefaultReleaseUrl below. Then collaborators can just run:
#       .\scripts\bootstrap.ps1
#
# Alternative:
#   Keep the script unchanged and set env:AUDIOSUB_WEBRTC_SDK_URL on each
#   collaborator machine.
#
# GitHub release asset URL example:
#   https://github.com/<user>/<repo>/releases/download/<tag>/webrtc-sdk-win-x64-release-<commit>.zip
# =====================================================================
$DefaultReleaseUrl = "https://github.com/NJUPTzza/AudioSub/releases/download/webrtc-sdk-v1.0.0/webrtc-sdk-win-x64-release-771e6489c9.zip"
$EnvReleaseUrl = [Environment]::GetEnvironmentVariable("AUDIOSUB_WEBRTC_SDK_URL")
$ResolvedDefaultUrl = if (-not [string]::IsNullOrWhiteSpace($EnvReleaseUrl)) {
    $EnvReleaseUrl
} else {
    $DefaultReleaseUrl
}

function Test-SdkReady {
    return (Test-Path $LibPath) -and (Test-Path $VersionPath)
}

function Expand-SdkArchive {
    param(
        [string]$ArchivePath,
        [string]$DestinationRoot
    )

    $tempDir = Join-Path $env:TEMP ("audiosub-webrtc-sdk-" + [guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Force -Path $tempDir | Out-Null
    try {
        Expand-Archive -Path $ArchivePath -DestinationPath $tempDir -Force

        # Support zip layouts:
        #   webrtc-sdk/...
        #   or flat lib/include at archive root
        $candidates = @()
        $candidates += Get-ChildItem $tempDir -Directory
        if (Test-Path (Join-Path $tempDir "lib/webrtc.lib")) {
            $candidates = @([PSCustomObject]@{ FullName = $tempDir })
        }

        $root = $null
        foreach ($c in $candidates) {
            if (Test-Path (Join-Path $c.FullName "lib/webrtc.lib")) {
                $root = $c.FullName
                break
            }
        }
        if (-not $root) {
            throw "Archive does not contain lib/webrtc.lib"
        }

        if (Test-Path $DestinationRoot) {
            Remove-Item -Recurse -Force $DestinationRoot
        }
        New-Item -ItemType Directory -Force -Path (Split-Path $DestinationRoot) | Out-Null
        Move-Item $root $DestinationRoot
    } finally {
        Remove-Item -Recurse -Force $tempDir -ErrorAction SilentlyContinue
    }
}

function Install-FromDirectory {
    param([string]$SourceDir)

    $src = (Resolve-Path $SourceDir).Path
    if (-not (Test-Path (Join-Path $src "lib/webrtc.lib"))) {
        throw "Invalid SDK directory (missing lib/webrtc.lib): $src"
    }

    if (Test-Path $SdkDir) {
        Remove-Item -Recurse -Force $SdkDir
    }
    Copy-Item -Recurse $src $SdkDir
}

Write-Host "===== fetch-webrtc-sdk =====" -ForegroundColor Cyan

if ((Test-SdkReady) -and -not $Force) {
    Write-Host "SDK already present: $SdkDir" -ForegroundColor Green
    Get-Content $VersionPath | ForEach-Object { Write-Host "  $_" }
    Write-Host ""
    Write-Host "Use -Force to re-download/reinstall."
    exit 0
}

if (-not [string]::IsNullOrWhiteSpace($LocalPath)) {
    Write-Host "Installing from local path: $LocalPath" -ForegroundColor Cyan
    Install-FromDirectory $LocalPath
}
elseif (-not [string]::IsNullOrWhiteSpace($Url)) {
    Write-Host "Downloading: $Url" -ForegroundColor Cyan
    $zipPath = Join-Path $env:TEMP ("audiosub-webrtc-sdk-" + [guid]::NewGuid().ToString("N") + ".zip")
    try {
        Invoke-WebRequest -Uri $Url -OutFile $zipPath -UseBasicParsing
        Expand-SdkArchive -ArchivePath $zipPath -DestinationRoot $SdkDir
    } finally {
        Remove-Item $zipPath -ErrorAction SilentlyContinue
    }
}
elseif (-not [string]::IsNullOrWhiteSpace($ResolvedDefaultUrl)) {
    if (-not [string]::IsNullOrWhiteSpace($EnvReleaseUrl)) {
        Write-Host "Downloading release from env:AUDIOSUB_WEBRTC_SDK_URL" -ForegroundColor Cyan
    } else {
        Write-Host "Downloading default release from script config" -ForegroundColor Cyan
    }
    Write-Host "  $ResolvedDefaultUrl"
    & $PSCommandPath -Url $ResolvedDefaultUrl -Force:$Force
    exit $LASTEXITCODE
}
else {
    Write-Host "WebRTC SDK not found." -ForegroundColor Yellow
    Write-Host ""
    Write-Host "Choose one:" -ForegroundColor Cyan
    Write-Host "  1) Maintainer: pack locally"
    Write-Host "       .\scripts\pack-webrtc-sdk.ps1"
    Write-Host ""
    Write-Host "  2) Maintainer: pack + zip, upload to GitHub Releases"
    Write-Host "       .\scripts\pack-webrtc-sdk.ps1 -Zip"
    Write-Host ""
    Write-Host "  3) Collaborator: download from Release URL"
    Write-Host "       .\scripts\fetch-webrtc-sdk.ps1 -Url <release-zip-url>"
    Write-Host ""
    Write-Host "  4) Team default: configure one of these once"
    Write-Host "       scripts\fetch-webrtc-sdk.ps1  (variable `$DefaultReleaseUrl)"
    Write-Host "       env:AUDIOSUB_WEBRTC_SDK_URL"
    Write-Host ""
    Write-Host "  5) Developer: link existing SDK folder"
    Write-Host "       .\scripts\fetch-webrtc-sdk.ps1 -LocalPath <sdk-dir>"
    exit 2
}

if (-not (Test-SdkReady)) {
    throw "SDK installation failed: $SdkDir"
}

Write-Host ""
Write-Host "SDK ready: $SdkDir" -ForegroundColor Green
Get-Content $VersionPath | ForEach-Object { Write-Host "  $_" }
$libMb = (Get-Item $LibPath).Length / 1MB
Write-Host ("  lib/webrtc.lib : {0:N2} MB" -f $libMb)
