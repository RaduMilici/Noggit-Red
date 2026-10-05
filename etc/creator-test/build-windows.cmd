@echo off
setlocal
cd /d "%~dp0\..\.."
where py >nul 2>nul
if errorlevel 1 goto python
py -3 "%~dp0build_windows.py" %*
goto finished
:python
where python >nul 2>nul
if errorlevel 1 goto missing
python "%~dp0build_windows.py" %*
:finished
if errorlevel 1 goto failed
echo.
echo Packaging completed successfully.
pause
exit /b 0
:missing
echo Python 3 is required on this build machine. Install it and rerun this script.
:failed
echo.
echo Packaging failed. See the error above.
pause
exit /b 1
