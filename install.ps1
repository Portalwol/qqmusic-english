# Installs the QQ Music English add-on, or removes it with -Uninstall.
#   version.dll  -> the QQ Music folder, next to QQMusic.exe (Windows asks for admin
#                   approval when that folder is in Program Files)
#   tables\*     -> %LOCALAPPDATA%\QQMusicEN, the translations the add-on reads
# QQ Music's own files are not changed. Re-run after a QQ Music update.
# Usage: double-click install.cmd / uninstall.cmd, or
#        install.ps1 [-Uninstall] [-Path "<folder that has QQMusic.exe>"]
param([switch]$Uninstall, [string]$Path, [switch]$Elevated)

$ErrorActionPreference = "Stop"
$here = $PSScriptRoot
$data = Join-Path $env:LOCALAPPDATA "QQMusicEN"

function Find-QQMusic {
    if ($Path) {
        if (Test-Path (Join-Path $Path "QQMusic.exe")) { return $Path.TrimEnd("\") }
        throw "QQMusic.exe is not in $Path."
    }
    $found = @()
    foreach ($key in "HKLM:\SOFTWARE\WOW6432Node\Tencent\QQMusic", "HKLM:\SOFTWARE\Tencent\QQMusic") {
        $found += (Get-ItemProperty $key -ErrorAction SilentlyContinue).Install
    }
    foreach ($key in "HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\QQMusic",
                     "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\QQMusic",
                     "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\QQMusic") {
        $found += (Get-ItemProperty $key -ErrorAction SilentlyContinue).InstallLocation
    }
    $found += "${env:ProgramFiles(x86)}\Tencent\QQMusic", "$env:ProgramFiles\Tencent\QQMusic"
    foreach ($dir in $found) {
        if ($dir -and (Test-Path (Join-Path $dir "QQMusic.exe"))) { return $dir.TrimEnd("\") }
    }
    throw "QQ Music was not found. Run install.ps1 -Path `"<folder that has QQMusic.exe>`"."
}

# Only ever overwrite or delete a version.dll that is this add-on.
function Test-Addon($file) {
    $bytes = [IO.File]::ReadAllBytes($file)
    foreach ($skip in 0, 1) {
        $text = [Text.Encoding]::Unicode.GetString($bytes, $skip, $bytes.Length - $skip)
        if ($text.Contains("\QQMusicEN")) { return $true }
    }
    return $false
}

function Assert-NotForeign($app) {
    $addon = Join-Path $app "version.dll"
    if ((Test-Path $addon) -and -not (Test-Addon $addon)) {
        throw "$addon exists and is not this add-on; leaving it alone."
    }
}

# The part that writes to the QQ Music folder.
function Set-Addon($app) {
    Assert-NotForeign $app
    $addon = Join-Path $app "version.dll"
    if ($Uninstall) {
        if (Test-Path $addon) { Remove-Item $addon -Force }
    } else {
        Copy-Item (Join-Path $here "version.dll") $addon -Force
    }
}

if ($Elevated) {
    # This window closes by itself on success; the window that started it reports the result.
    try { Set-Addon $Path } catch { Write-Host $_ -ForegroundColor Red; Read-Host "Press Enter to close" | Out-Null; exit 1 }
    exit 0
}

$app = Find-QQMusic
Write-Host "QQ Music: $app"
if (Get-Process -Name "QQMusic*" -ErrorAction SilentlyContinue) {
    throw "QQ Music is running. Close it first (right-click the tray icon, Exit), then run this again."
}
Assert-NotForeign $app

if (-not $Uninstall) {
    if (-not (Test-Path (Join-Path $here "version.dll"))) { throw "version.dll is missing next to install.ps1. Use the release zip, or run build.ps1 first." }
    New-Item -ItemType Directory -Force $data | Out-Null
    Copy-Item (Join-Path $here "tables\*") $data -Force
    Get-ChildItem $here -Recurse -File | Unblock-File
}

try {
    Set-Addon $app
} catch [UnauthorizedAccessException], [System.Management.Automation.ActionPreferenceStopException] {
    # The QQ Music folder needs admin rights: do just that part in an elevated window.
    $argList = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$PSCommandPath`"", "-Elevated", "-Path", "`"$app`"")
    if ($Uninstall) { $argList += "-Uninstall" }
    try { $p = Start-Process powershell -Verb RunAs -Wait -PassThru -ArgumentList $argList }
    catch { throw "Admin approval is needed to write to $app." }
    if ($p.ExitCode -ne 0) { throw "Writing to $app failed." }
}

if ($Uninstall) {
    Remove-Item $data -Recurse -Force -ErrorAction SilentlyContinue
    Write-Host "QQ Music English add-on removed." -ForegroundColor Green
} else {
    Write-Host "QQ Music English add-on installed. Start QQ Music to see it." -ForegroundColor Green
}
