@echo off
rem Launches spud windowed in Tamriel, overlooking Riverwood. Always builds first, so it runs what's on main; imports if needed.
rem Extra arguments go straight to spud.exe, e.g.:  launch.bat --cell 5 -2   or   launch.bat --world whiterunworld
rem Everything runs through the machine gate (tools\gate): if a perf test is running, this waits for it
rem to finish before building, importing or opening the window, and no perf test starts while it is open.
setlocal
set "ROOT=%~dp0"
set "EXE=%ROOT%build\release\bin\spud.exe"
set "SKYTOOL=%ROOT%build\release\bin\skytool.exe"
set "CACHE=%LOCALAPPDATA%\spud\cache\skyrim"
set "GATE=%ROOT%tools\gate\gate.py"
set "SPUD_GATE_WHO=you (launch.bat)"

where python >nul 2>nul
if errorlevel 1 (
    echo Python 3 is needed to run spud's tools - install it from python.org and try again.
    goto failed
)
echo Building the latest main...
call "%ROOT%scripts\build.cmd" release
if errorlevel 1 (
    echo.
    echo The build failed. If spud is already open, close it and run this again.
    goto failed
)
if not exist "%CACHE%\worlds\tamriel.osw" (
    echo No imported game data yet - importing your Skyrim install, this takes a few minutes...
    python -I "%GATE%" run shared --what "skytool import" -- "%SKYTOOL%" import
    if errorlevel 1 goto failed
)

echo.
echo   Hold right mouse or Tab to look around    WASD move    Space/E up    Ctrl/Q down
echo   Shift faster    mouse wheel changes speed    F1 debug UI with FPS and settings
echo   N walks on foot with Skyrim's keys and back (re-import to make buildings solid)
echo   If a perf test is using the machine, the window opens as soon as it finishes.
echo.
python -I "%GATE%" run shared --what "spud window (Riverwood)" -- "%EXE%" --world tamriel --pos 18432 -47104 +400 --yaw 45 --pitch 5 %*
if errorlevel 1 goto failed
exit /b 0

:failed
echo.
echo Something went wrong - see the messages above.
pause
exit /b 1
