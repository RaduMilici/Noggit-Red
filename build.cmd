@echo off
rem Fast Windows build of Noggit (Creator): MSVC x64 + Ninja + sccache.
rem
rem   build.cmd             configure on first run, then build noggit (only what changed)
rem   build.cmd configure   re-run CMake (after adding or removing source files)
rem   build.cmd clean       delete the build folder and start over
rem
rem Works from any terminal (PowerShell, cmd, double-click): it sets up the x64 compiler itself.
rem Needs: Visual Studio 2022 C++ build tools, Qt 5.15.2 msvc2019_64, and once:
rem   winget install Ninja-build.Ninja
rem   winget install Mozilla.sccache
setlocal EnableDelayedExpansion
cd /d "%~dp0"

set "BUILD_DIR=build-ninja"
if not defined QT_DIR set "QT_DIR=C:/Qt/5.15.2/msvc2019_64"
set "MYSQL_ROOT=%CD%\build-runtime-cache\windows-vs17\tortoise-source\dep\windows"

if /i "%~1"=="clean" (
  echo Deleting %BUILD_DIR% ...
  if exist "%BUILD_DIR%" rmdir /s /q "%BUILD_DIR%"
)

rem --- 1. MSVC x64 compiler environment (skipped when already in an x64 developer prompt) ---
where cl >nul 2>nul
if not errorlevel 1 goto :have_msvc
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :no_vs
set "VSINSTALL="
rem The extra outer quotes survive cmd's quote stripping ("Program Files (x86)" has parentheses).
for /f "usebackq delims=" %%i in (`""%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath"`) do set "VSINSTALL=%%i"
if not defined VSINSTALL goto :no_vs
echo Setting up the x64 compiler from %VSINSTALL% ...
call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" >nul
where cl >nul 2>nul
if errorlevel 1 goto :no_vs
:have_msvc

rem --- 2. Ninja and sccache (winget installs them without always updating PATH) ---
call :find_tool ninja.exe Ninja-build.Ninja_ NINJA
if not defined NINJA goto :no_ninja
call :find_tool sccache.exe Mozilla.sccache_ SCCACHE
if defined SCCACHE (
  set "LAUNCHER=-DCMAKE_C_COMPILER_LAUNCHER=!SCCACHE:\=/! -DCMAKE_CXX_COMPILER_LAUNCHER=!SCCACHE:\=/!"
  rem A bigger cache than the 10 GB default: Noggit plus its libraries fill it quickly.
  if not defined SCCACHE_CACHE_SIZE set "SCCACHE_CACHE_SIZE=30G"
) else (
  echo sccache not found: building without a compiler cache. Install it with: winget install Mozilla.sccache
  set "LAUNCHER="
)

rem --- 3. Configure (first run, after "clean"/"configure", or when a previous configure failed) ---
if /i "%~1"=="configure" goto :configure
if not exist "%BUILD_DIR%\build.ninja" goto :configure
goto :build

:configure
if exist "%BUILD_DIR%\CMakeCache.txt" if not exist "%BUILD_DIR%\build.ninja" (
  rem A failed configure remembers the missing compiler: start that folder over.
  rmdir /s /q "%BUILD_DIR%"
)
if not exist "%MYSQL_ROOT%\include\mysql\mysql.h" goto :no_mysql
set "RUNTIME_BUNDLE="
for /d %%d in ("%CD%\build-runtime\windows-*") do if exist "%%d\Runtime\creator-runtime.json" set "RUNTIME_BUNDLE=%%d\Runtime"
set "BUNDLE_ARG="
if defined RUNTIME_BUNDLE set "BUNDLE_ARG=-DCREATOR_RUNTIME_BUNDLE=!RUNTIME_BUNDLE:\=/!"
echo Configuring %BUILD_DIR% ...
cmake -S . -B "%BUILD_DIR%" -G Ninja ^
  -DCMAKE_MAKE_PROGRAM="%NINJA:\=/%" ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_PREFIX_PATH="%QT_DIR%" ^
  -DUSE_SQL=ON ^
  -DMYSQL_INCLUDE_DIR="%MYSQL_ROOT:\=/%/include/mysql" ^
  -DMYSQL_LIBRARY="%MYSQL_ROOT:\=/%/lib/x64_release/libmysql.lib" ^
  %BUNDLE_ARG% %LAUNCHER%
if errorlevel 1 goto :failed

:build
echo Building noggit ...
set "START=%TIME%"
cmake --build "%BUILD_DIR%" --target noggit
if errorlevel 1 goto :failed
echo.
echo Done: %BUILD_DIR%\bin\noggit.exe   (started %START%, finished %TIME%)
if defined SCCACHE "%SCCACHE%" --show-stats 2>nul | findstr /i /c:"Cache hits rate" /c:"Compile requests executed"
exit /b 0

rem --- helpers ---
:find_tool
rem %1 exe name, %2 winget package folder prefix, %3 variable to set
set "%3="
for /f "delims=" %%p in ('where %1 2^>nul') do if not defined %3 set "%3=%%p"
if defined %3 exit /b 0
for /d %%d in ("%LOCALAPPDATA%\Microsoft\WinGet\Packages\%2*") do (
  for /f "delims=" %%p in ('dir /s /b "%%d\%1" 2^>nul') do if not defined %3 set "%3=%%p"
)
exit /b 0

:no_vs
echo ERROR: Could not find the Visual Studio 2022 C++ x64 build tools.
echo Install "Desktop development with C++" (or the C++ Build Tools) from the Visual Studio Installer.
exit /b 1
:no_ninja
echo ERROR: Ninja not found. Install it with: winget install Ninja-build.Ninja
exit /b 1
:no_mysql
echo ERROR: MariaDB client headers not found in %MYSQL_ROOT%
echo They come with the prepared Creator runtime (see etc\creator-test\PACKAGING.md).
exit /b 1
:failed
echo.
echo BUILD FAILED. See the messages above.
exit /b 1
