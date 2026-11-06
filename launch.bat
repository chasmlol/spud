@echo off
rem Builds Spud, imports your game if needed, and opens it in Tamriel overlooking Riverwood.
rem Extra arguments go straight to spud.exe, e.g.:  launch.bat --cell 5 -2   or   launch.bat --world whiterunworld
setlocal
set "ROOT=%~dp0"
set "EXE=%ROOT%build\release\bin\spud.exe"
set "SKYTOOL=%ROOT%build\release\bin\skytool.exe"
set "CACHE=%LOCALAPPDATA%\spud\cache\skyrim"

echo Building...
call "%ROOT%scripts\build.cmd" release
if errorlevel 1 (
    echo.
    echo The build failed. If Spud is already open, close it and run this again.
    goto failed
)
if not exist "%CACHE%\worlds\tamriel.osw" (
    echo No imported game data yet - importing your Skyrim install, this takes a few minutes...
    "%SKYTOOL%" import
    if errorlevel 1 goto failed
)

"%EXE%" --world tamriel --pos 18432 -47104 +400 --yaw 45 --pitch 5 %*
if errorlevel 1 goto failed
exit /b 0

:failed
echo.
echo Something went wrong - see the messages above.
pause
exit /b 1
