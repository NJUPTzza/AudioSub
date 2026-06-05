<#
.SYNOPSIS
  Start signaling server + GUI B + GUI A for local testing.

.EXAMPLE
  .\scripts\start-gui.ps1
#>

param(
    [string]$Config = "Release",
    [int]$DelayMs = 400
)

$ErrorActionPreference = "Stop"

$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$GuiExe = Join-Path $RepoRoot "build\gui\$Config\audiosub_gui.exe"
$Signaling = Join-Path $RepoRoot "signaling\server.py"

if (-not (Test-Path $GuiExe)) {
    throw "GUI not found: $GuiExe`nRun .\scripts\build.ps1 first."
}

if (-not (Test-Path $Signaling)) {
    throw "Signaling script not found: $Signaling"
}

Write-Host "Starting signaling server..."
Start-Process powershell -WorkingDirectory $RepoRoot -ArgumentList @(
    "-NoExit",
    "-Command",
    "python '$Signaling'"
)

Start-Sleep -Milliseconds $DelayMs

Write-Host "Starting GUI B (receiver)..."
Start-Process -FilePath $GuiExe -WorkingDirectory $RepoRoot -ArgumentList @("--id", "B")

Start-Sleep -Milliseconds $DelayMs

Write-Host "Starting GUI A (speaker)..."
Start-Process -FilePath $GuiExe -WorkingDirectory $RepoRoot -ArgumentList @("--id", "A")

Write-Host ""
Write-Host "All started."
Write-Host "Note: GUI is silent for ~10s while loading whisper model, then the window appears."
Write-Host "Close the signaling PowerShell window to stop the server."
