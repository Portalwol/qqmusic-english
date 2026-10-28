@echo off
rem Removes the QQ Music English add-on. Close QQ Music first.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" -Uninstall %*
pause
