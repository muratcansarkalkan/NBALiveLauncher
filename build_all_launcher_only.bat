@echo off
setlocal

cd /d "%~dp0"

where py >nul 2>&1
if %errorlevel%==0 (
    py build_all_launcher_only.py %*
) else (
    python build_all_launcher_onlybuild_all_launcher_only.py %*
)

set "BUILD_EXIT=%ERRORLEVEL%"

if not "%BUILD_EXIT%"=="0" (
    echo.
    echo Build failed with exit code %BUILD_EXIT%.
)

exit /b %BUILD_EXIT%
