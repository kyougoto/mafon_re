@echo off
rem Convenience launcher for build.ps1 (bypasses PowerShell execution policy,
rem which blocks unsigned local scripts on some hosts).
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
exit /b %ERRORLEVEL%
