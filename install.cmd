@echo off
rem Installs the QQ Music English add-on. Close QQ Music first.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" %*
pause
