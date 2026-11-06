@echo off
rem Image-clarity audit: static checks plus headless history checks. See docs\RENDERING.md.
rem   scripts\audit.cmd [--static-only] [--build DIR]
rem Takes a shared pass from the gate (tools\gate): it waits while a perf run holds the machine.
python -I "%~dp0..\tools\gate\gate.py" run shared --what "audit %*" -- python -I "%~dp0..\tools\audit\history_audit.py" %*
