@echo off
rem Runs the perf harness against the release build. See docs\PERFORMANCE.md.
rem   scripts\perf.cmd [--scenario NAME] [--full] [--windowed] [--update-baseline] [--build DIR]
rem The run takes an exclusive pass from the gate (tools\gate): it waits for builds, checks and game
rem windows to finish, nothing new starts while it runs, and if heavy work starts outside the gate
rem anyway, the run is discarded and re-queued (up to twice).
python -I "%~dp0..\tools\gate\gate.py" run exclusive --retries 2 --what "perf %*" -- python -I "%~dp0..\tools\perf\perf.py" %*
