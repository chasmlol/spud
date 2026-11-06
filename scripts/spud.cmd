@echo off
rem Runs this checkout's release spud.exe (headless checks, screenshots, scripts) under a shared pass
rem from the gate (tools\gate), so it waits while a perf run holds the machine. Arguments pass through.
python -I "%~dp0..\tools\gate\gate.py" run shared --what "spud" -- "%~dp0..\build\release\bin\spud.exe" %*
