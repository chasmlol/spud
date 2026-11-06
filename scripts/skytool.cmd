@echo off
rem Runs this checkout's release skytool.exe (imports, scans) under a shared pass from the gate
rem (tools\gate), so it waits while a perf run holds the machine. Arguments pass through.
python -I "%~dp0..\tools\gate\gate.py" run shared --what "skytool %~1" -- "%~dp0..\build\release\bin\skytool.exe" %*
