# Builds dist\QQMusicEnglish: the add-on (version.dll), its translation tables and the installer.
# Needs Visual Studio or Build Tools with "Desktop development with C++", and Python 3.
# -Zip also writes dist\QQMusicEnglish.zip, the file attached to a release.
param([switch]$Zip)

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$out = Join-Path $root "dist\QQMusicEnglish"
$obj = Join-Path $root "dist\obj"
Remove-Item $out -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $out, $obj | Out-Null

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "Visual Studio not found. Install Visual Studio or Build Tools with 'Desktop development with C++'." }
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "Visual Studio has no C++ build tools. Add 'Desktop development with C++' in the Visual Studio Installer." }
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvarsall.bat"

# QQ Music is a 32-bit app, so the add-on is built for x86.
$src = (Get-ChildItem (Join-Path $root "src\*.cpp") | ForEach-Object { "`"$($_.FullName)`"" }) -join " "
$cl = "cl /nologo /LD /O2 /EHsc /std:c++17 /MT /W3 /utf-8 $src /Fe:`"$out\version.dll`" shell32.lib ole32.lib user32.lib gdi32.lib winhttp.lib"
cmd /c "call `"$vcvars`" x86 >nul 2>nul &&cd /d `"$obj`" && $cl"
if ($LASTEXITCODE -ne 0) { throw "Building version.dll failed." }
Remove-Item "$out\version.lib", "$out\version.exp" -ErrorAction SilentlyContinue

python -X utf8 (Join-Path $root "tools\build_tables.py") (Join-Path $out "tables")
if ($LASTEXITCODE -ne 0) { throw "Building the translation tables failed." }

Copy-Item (Join-Path $root "install.ps1"), (Join-Path $root "install.cmd"), (Join-Path $root "uninstall.cmd"), (Join-Path $root "LICENSE") $out
Write-Host "Built $out" -ForegroundColor Green

if ($Zip) {
    $zipFile = Join-Path $root "dist\QQMusicEnglish.zip"
    Remove-Item $zipFile -ErrorAction SilentlyContinue
    Compress-Archive -Path $out -DestinationPath $zipFile
    Write-Host "Packed $zipFile" -ForegroundColor Green
}
