@echo off
rem Usage: scripts\build.cmd [preset] [build-dir]
rem   preset    debug | release (default: release)
rem   build-dir optional override, e.g. build\mywork, so parallel builds don't share a tree
rem Builds take a shared pass from the gate (tools\gate): they pause while a perf run holds the machine.
setlocal
if defined SPUD_GATE goto gated
python -I "%~dp0..\tools\gate\gate.py" run shared --what "build %~1 %~2" -- cmd /c "%~f0" %*
exit /b %errorlevel%
:gated
set PRESET=%~1
if "%PRESET%"=="" set PRESET=release
set BUILD_DIR=%~2
if "%BUILD_DIR%"=="" set BUILD_DIR=build\%PRESET%
if "%VCPKG_ROOT%"=="" set VCPKG_ROOT=C:\vcpkg

for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSINSTALL=%%i
if "%VSINSTALL%"=="" (echo Visual Studio with C++ tools not found & exit /b 1)
call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

cd /d "%~dp0.."
cmake --preset %PRESET% -B "%BUILD_DIR%" || exit /b 1
cmake --build "%BUILD_DIR%" || exit /b 1
