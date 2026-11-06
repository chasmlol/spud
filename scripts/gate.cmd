@echo off
rem The machine gate: one queue for heavy work so perf runs never overlap anything. See tools\gate\gate.py.
rem   scripts\gate.cmd status
rem   scripts\gate.cmd run shared^|exclusive [--who NAME] [--what TEXT] -- COMMAND ...
python -I "%~dp0..\tools\gate\gate.py" %*
