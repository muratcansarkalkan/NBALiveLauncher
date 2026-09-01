@echo off
setlocal
cd /d "%~dp0"
where py >nul 2>nul
if %errorlevel%==0 (
    py -3 build_all.py --no-pause %*
) else (
    python build_all.py --no-pause %*
)
set BUILD_EXIT=%errorlevel%
pause
exit /b %BUILD_EXIT%
