param(
    [string]$Config = "Release",
    [switch]$Clean
)

$ErrorActionPreference = "Stop"
$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$BuildDir = Join-Path $RepoRoot "build"
$SdkLib = Join-Path $RepoRoot "third_party/webrtc-sdk/lib/webrtc.lib"

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

    $installationVersion = & $vswhere -latest -products * `
        -requires Microsoft.Component.MSBuild `
        -property installationVersion

    [PSCustomObject]@{
        InstallationPath = $installationPath.Trim()
        InstallationVersion = ($installationVersion | Out-String).Trim()
    }
}

function Resolve-ToolPath {
    param(
        [string]$CommandName,
        [string[]]$CandidatePaths = @()
    )

    $cmd = Get-Command $CommandName -ErrorAction SilentlyContinue
    if ($cmd) {
        return $cmd.Source
    }

    foreach ($path in $CandidatePaths) {
        if ($path -and (Test-Path $path)) {
            return $path
        }
    }

    return $null
}

function Get-CMakePath {
    param([object]$VsInfo)

    $candidates = @()
    if ($VsInfo) {
        $candidates += Join-Path $VsInfo.InstallationPath "CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
        $candidates += Join-Path $VsInfo.InstallationPath "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    }

    return Resolve-ToolPath -CommandName "cmake" -CandidatePaths $candidates
}

function Get-MSBuildPath {
    param([object]$VsInfo)

    $candidates = @()
    if ($VsInfo) {
        $candidates += Join-Path $VsInfo.InstallationPath "MSBuild\Current\Bin\MSBuild.exe"
    }

    return Resolve-ToolPath -CommandName "MSBuild" -CandidatePaths $candidates
}

if (-not (Test-Path $SdkLib)) {
    Write-Host "WebRTC SDK not found." -ForegroundColor Red
    Write-Host "Run one of:" -ForegroundColor Yellow
    Write-Host "  .\scripts\fetch-webrtc-sdk.ps1 -Url <release-zip-url>"
    Write-Host "  .\scripts\fetch-webrtc-sdk.ps1 -LocalPath <sdk-dir>"
    Write-Host "  .\scripts\pack-webrtc-sdk.ps1            (maintainer)"
    exit 2
}

$vsInfo = Get-VisualStudioInfo
$cmake = Get-CMakePath -VsInfo $vsInfo
$msbuild = Get-MSBuildPath -VsInfo $vsInfo

if (-not $cmake) {
    Write-Host "CMake not found." -ForegroundColor Red
    Write-Host "Install one of the following and retry:" -ForegroundColor Yellow
    Write-Host "  - CMake and add it to PATH"
    Write-Host "  - Visual Studio 2022 / Build Tools with C++ and CMake components"
    exit 3
}

Write-Host "Using cmake   : $cmake" -ForegroundColor DarkCyan
if ($vsInfo) {
    Write-Host "Visual Studio : $($vsInfo.InstallationPath)" -ForegroundColor DarkCyan
}
if ($msbuild) {
    Write-Host "Using MSBuild : $msbuild" -ForegroundColor DarkCyan
}

if ($Clean -and (Test-Path $BuildDir)) {
    Write-Host "Cleaning $BuildDir ..." -ForegroundColor Yellow
    try {
        Remove-Item -Recurse -Force $BuildDir
    } catch {
        Write-Host "Failed to clean build directory. Some files may be locked by Visual Studio, Explorer, or a running process." -ForegroundColor Red
        Write-Host "Close processes using '$BuildDir' and retry, or run without -Clean." -ForegroundColor Yellow
        throw
    }
}

if (-not (Test-Path $BuildDir)) {
    New-Item -ItemType Directory $BuildDir | Out-Null
}

Push-Location $BuildDir
try {
    Write-Host "===== cmake configure =====" -ForegroundColor Cyan
    & $cmake .. -G "Visual Studio 17 2022" -A x64
    if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }

    Write-Host ""
    Write-Host "===== cmake build ($Config) =====" -ForegroundColor Cyan
    & $cmake --build . --config $Config
    if ($LASTEXITCODE -ne 0) { throw "cmake build failed" }

    $Exe = Join-Path $BuildDir "client\$Config\audiosub_client.exe"
    if (Test-Path $Exe) {
        Write-Host ""
        Write-Host "Built: $Exe" -ForegroundColor Green
        Write-Host ("Size : {0:N2} MB" -f ((Get-Item $Exe).Length / 1MB))
    }
} finally {
    Pop-Location
}
