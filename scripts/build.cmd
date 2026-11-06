@echo off
rem Usage: scripts\build.cmd [preset] [build-dir]
rem   preset    debug | release (default: release)
rem   build-dir optional override
setlocal
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
