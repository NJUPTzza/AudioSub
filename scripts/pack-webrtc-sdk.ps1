<#
.SYNOPSIS
  Package a distributable WebRTC SDK for AudioSub collaborators.

.DESCRIPTION
  Creates third_party/webrtc-sdk layout (or a zip under dist/) containing:
    lib/webrtc.lib
    include/   (header tree mirroring webrtc/src)
    VERSION.txt

  Run this on a machine that has already built WebRTC Release with the
  project's recommended gn args (use_custom_libcxx=false).

.EXAMPLE
  .\scripts\pack-webrtc-sdk.ps1
  .\scripts\pack-webrtc-sdk.ps1 -WebRtcSrc E:\webrtc\src -Zip
  .\scripts\pack-webrtc-sdk.ps1 -OutputDir E:\Cpp\Codes\AudioSub\third_party\webrtc-sdk
#>

param(
    [string]$WebRtcSrc = "E:/webrtc/src",
    [string]$OutputDir = "",
    [switch]$Zip,
    [string]$Version = "1.0.0"
)

$ErrorActionPreference = "Stop"

$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$WebRtcSrc = (Resolve-Path $WebRtcSrc).Path
$LibPath = Join-Path $WebRtcSrc "out/Release/obj/webrtc.lib"

if (-not (Test-Path $LibPath)) {
    throw "webrtc.lib not found: $LibPath`nBuild WebRTC first: ninja -C out/Release webrtc"
}

if ([string]::IsNullOrWhiteSpace($OutputDir)) {
    $shortCommit = ""
    Push-Location $WebRtcSrc
    try {
        $shortCommit = (git rev-parse --short HEAD).Trim()
    } finally {
        Pop-Location
    }
    if ($Zip) {
        $OutputDir = Join-Path $RepoRoot "dist/webrtc-sdk-win-x64-release-$shortCommit"
    } else {
        $OutputDir = Join-Path $RepoRoot "third_party/webrtc-sdk"
    }
} else {
    $OutputDir = (Resolve-Path -LiteralPath $OutputDir -ErrorAction SilentlyContinue)
    if (-not $OutputDir) {
        $OutputDir = Join-Path $RepoRoot $OutputDir
    }
}

$includeDir = Join-Path $OutputDir "include"
$libDir = Join-Path $OutputDir "lib"

Write-Host "===== pack-webrtc-sdk =====" -ForegroundColor Cyan
Write-Host "WebRTC src : $WebRtcSrc"
Write-Host "Output     : $OutputDir"

if (Test-Path $OutputDir) {
    Write-Host "Removing existing output..." -ForegroundColor Yellow
    Remove-Item -Recurse -Force $OutputDir
}
New-Item -ItemType Directory -Force -Path $libDir | Out-Null
New-Item -ItemType Directory -Force -Path $includeDir | Out-Null

Write-Host "[1/3] Copy webrtc.lib ..." -ForegroundColor Cyan
Copy-Item $LibPath (Join-Path $libDir "webrtc.lib")

Write-Host "[2/3] Copy headers (this may take 1-2 min) ..." -ForegroundColor Cyan
# Copy header files only, preserving directory layout under webrtc/src.
# Exclude build artifacts and tooling trees.
$excludeDirs = @("out", "tools", "testing", "build", ".git", "obj")
$excludeArgs = $excludeDirs | ForEach-Object { "/XD"; $_ }

$robocopyArgs = @(
    $WebRtcSrc, $includeDir,
    "/E", "/NFL", "/NDL", "/NJH", "/NJS", "/NC", "/NS",
    "/IF", "*.h", "*.hpp", "*.inc", "*.inl"
) + $excludeArgs

& robocopy @robocopyArgs | Out-Null
# robocopy exit codes 0-7 are success variants
if ($LASTEXITCODE -gt 7) {
    throw "robocopy failed with exit code $LASTEXITCODE"
}

Write-Host "[3/3] Write VERSION.txt ..." -ForegroundColor Cyan
Push-Location $WebRtcSrc
try {
    $commit = (git rev-parse HEAD).Trim()
    $branch = (git branch --show-current).Trim()
} finally {
    Pop-Location
}

$gnArgs = @(
    'is_debug=false',
    'is_clang=true',
    'use_custom_libcxx=false',
    'is_component_build=false',
    'rtc_use_h264=true',
    'ffmpeg_branding="Chrome"',
    'rtc_include_tests=false',
    'rtc_build_examples=false',
    'treat_warnings_as_errors=false',
    'proprietary_codecs=true',
    'rtc_enable_protobuf=false',
    'target_cpu="x64"'
) -join ' '

$versionText = @"
audiosub_webrtc_sdk_version=$Version
platform=win-x64
config=Release
webrtc_branch=$branch
webrtc_commit=$commit
gn_args=$gnArgs
built_utc=$(Get-Date -Format o)
"@

Set-Content -Path (Join-Path $OutputDir "VERSION.txt") -Value $versionText -Encoding UTF8

$libMb = (Get-Item (Join-Path $libDir "webrtc.lib")).Length / 1MB
$headerCount = (Get-ChildItem $includeDir -Recurse -File | Measure-Object).Count
Write-Host ""
Write-Host "Done." -ForegroundColor Green
Write-Host ("  lib/webrtc.lib : {0:N2} MB" -f $libMb)
Write-Host "  include/       : $headerCount header files"
Write-Host "  VERSION.txt    : commit $commit"

if ($Zip) {
    $distRoot = Join-Path $RepoRoot "dist"
    if (-not (Test-Path $distRoot)) {
        New-Item -ItemType Directory $distRoot | Out-Null
    }
    $zipPath = "$OutputDir.zip"
    if (Test-Path $zipPath) { Remove-Item -Force $zipPath }

    Write-Host ""
    Write-Host "Creating zip: $zipPath" -ForegroundColor Cyan
    # Prefer tar (more reliable with many small files than Compress-Archive)
    $tar = Get-Command tar -ErrorAction SilentlyContinue
    if ($tar) {
        Push-Location (Split-Path $OutputDir)
        try {
            $folderName = Split-Path $OutputDir -Leaf
            & tar -a -c -f $zipPath $folderName
            if ($LASTEXITCODE -ne 0) { throw "tar failed with exit code $LASTEXITCODE" }
        } finally {
            Pop-Location
        }
    } else {
        Compress-Archive -Path $OutputDir -DestinationPath $zipPath
    }

    $zipMb = (Get-Item $zipPath).Length / 1MB
    Write-Host ("Zip size: {0:N2} MB" -f $zipMb) -ForegroundColor Green
    Write-Host ""
    Write-Host "Upload this zip to GitHub Releases, then set the URL in:" -ForegroundColor Yellow
    Write-Host "  scripts/fetch-webrtc-sdk.ps1  (variable `$DefaultReleaseUrl)"
}
