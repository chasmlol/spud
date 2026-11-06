@echo off
rem Removes regenerable leftovers of finished work (old builds, stale private caches, old run folders).
rem   scripts\clean.cmd           list what would go
rem   scripts\clean.cmd --apply   remove it
rem See tools\clean\clean.py. The machine gate runs it by itself when the drive gets low.
python -I "%~dp0..\tools\clean\clean.py" %*
