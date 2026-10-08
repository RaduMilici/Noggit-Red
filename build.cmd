@echo off
rem Builds Noggit (Creator) and starts it. One command, nothing else to set up:
rem
rem   build.cmd           build what changed, then start Noggit
rem   build.cmd nostart   build only
rem   build.cmd clean     rebuild everything from scratch (your NPCs, quests and history are kept)
rem
rem It sets up the x64 compiler itself (any terminal, or double-click), re-runs CMake when source files
rem were added or removed, copies the DLLs Noggit needs, and links the local server runtime.
rem Your local database and Workspace live in .\Creator, outside the build folder, so "clean" never touches them.
rem
rem Needs once: Visual Studio 2022 C++ build tools, Qt 5.15.2 msvc2019_64, the prepared Creator runtime
rem (etc\creator-test\PACKAGING.md), and:  winget install Ninja-build.Ninja   winget install Mozilla.sccache
setlocal EnableDelayedExpansion
cd /d "%~dp0"

set "BUILD_DIR=build-ninja"
set "CREATOR_HOME=%CD%\Creator"
if not defined QT_DIR set "QT_DIR=C:/Qt/5.15.2/msvc2019_64"
set "MYSQL_ROOT=%CD%\build-runtime-cache\windows-vs17\tortoise-source\dep\windows"
set "RUNTIME_BUNDLE="
for /d %%d in ("%CD%\build-runtime\windows-*") do if exist "%%d\Runtime\creator-runtime.json" set "RUNTIME_BUNDLE=%%d\Runtime"

rem --- 0. Noggit must be closed: Windows will not let the build replace a running noggit.exe ---
tasklist /fi "imagename eq noggit.exe" 2>nul | find /i "noggit.exe" >nul
if not errorlevel 1 goto :running

if /i "%~1"=="clean" (
  echo Deleting %BUILD_DIR% ...
  rem Remove the link to your Creator data first, so deleting the build folder cannot reach it.
  if exist "%BUILD_DIR%\Creator" rmdir "%BUILD_DIR%\Creator"
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
echo Setting up the x64 compiler ...
call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" >nul
where cl >nul 2>nul
if errorlevel 1 goto :no_vs
:have_msvc

rem --- 2. Ninja and sccache (winget installs them without always updating PATH) ---
call :find_tool ninja.exe Ninja-build.Ninja_ NINJA
if not defined NINJA goto :no_ninja
call :find_tool sccache.exe Mozilla.sccache_ SCCACHE
set "LAUNCHER="
if defined SCCACHE set "LAUNCHER=-DCMAKE_C_COMPILER_LAUNCHER=!SCCACHE:\=/! -DCMAKE_CXX_COMPILER_LAUNCHER=!SCCACHE:\=/!"
if not defined SCCACHE echo Note: sccache not found, building without a compiler cache.
rem A bigger cache than the 10 GB default: Noggit plus its libraries fill it quickly.
if not defined SCCACHE_CACHE_SIZE set "SCCACHE_CACHE_SIZE=30G"

rem --- 3. Configure when needed: first run, after "clean", after a failed configure, or when source files
rem        were added or removed (CMake only picks up new files when it runs) ---
set "SOURCES=%TEMP%\noggit-build-sources.txt"
dir /s /b /a-d src\*.cpp src\*.hpp src\*.h src\*.c > "%SOURCES%" 2>nul
if not exist "%BUILD_DIR%\build.ninja" goto :configure
if not exist "%BUILD_DIR%\sources.txt" goto :configure
fc /b "%SOURCES%" "%BUILD_DIR%\sources.txt" >nul 2>nul
if errorlevel 1 goto :configure
goto :build

:configure
if exist "%BUILD_DIR%\CMakeCache.txt" if not exist "%BUILD_DIR%\build.ninja" rmdir /s /q "%BUILD_DIR%"
if not exist "%MYSQL_ROOT%\include\mysql\mysql.h" goto :no_mysql
set "BUNDLE_ARG="
if defined RUNTIME_BUNDLE set "BUNDLE_ARG=-DCREATOR_RUNTIME_BUNDLE=!RUNTIME_BUNDLE:\=/!"
echo Configuring ...
cmake -S . -B "%BUILD_DIR%" -G Ninja ^
  -DCMAKE_MAKE_PROGRAM="%NINJA:\=/%" ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_PREFIX_PATH="%QT_DIR%" ^
  -DUSE_SQL=ON ^
  -DMYSQL_INCLUDE_DIR="%MYSQL_ROOT:\=/%/include/mysql" ^
  -DMYSQL_LIBRARY="%MYSQL_ROOT:\=/%/lib/x64_release/libmysql.lib" ^
  %BUNDLE_ARG% %LAUNCHER%
if errorlevel 1 goto :failed
copy /y "%SOURCES%" "%BUILD_DIR%\sources.txt" >nul
`
:build
echo Building noggit ...
set "START=%TIME%"
cmake --build "%BUILD_DIR%" --target noggit
if errorlevel 1 goto :failed

rem --- 4. Make the build runnable ---
rem The SQL build links the MariaDB client: noggit.exe does not start without its DLL beside it.
copy /y "%MYSQL_ROOT%\lib\x64_release\libmysql.dll" "%BUILD_DIR%\bin\" >nul
rem Noggit looks for its Creator home next to bin: build-ninja\Creator -> .\Creator (your data, kept
rem across clean builds) whose Runtime links to the prepared local server.
if not exist "%CREATOR_HOME%" mkdir "%CREATOR_HOME%"
if not exist "%CREATOR_HOME%\Runtime\creator-runtime.json" if defined RUNTIME_BUNDLE (
  if exist "%CREATOR_HOME%\Runtime" rmdir "%CREATOR_HOME%\Runtime"
  mklink /J "%CREATOR_HOME%\Runtime" "%RUNTIME_BUNDLE%" >nul
)
if not exist "%BUILD_DIR%\Creator" mklink /J "%BUILD_DIR%\Creator" "%CREATOR_HOME%" >nul
if not exist "%CREATOR_HOME%\Runtime\creator-runtime.json" (
  echo Warning: no prepared Creator runtime in build-runtime\. Noggit starts without the local server.
)

echo.
echo Built in %START% - %TIME%: %BUILD_DIR%\bin\noggit.exe
if defined SCCACHE "%SCCACHE%" --show-stats 2>nul | findstr /i /c:"Cache hits rate"
if /i "%~1"=="nostart" exit /b 0
echo Starting Noggit ...
start "" /d "%CD%\%BUILD_DIR%\bin" "%CD%\%BUILD_DIR%\bin\noggit.exe"
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

:running
echo Noggit is running. Close it first, then run build.cmd again.
exit /b 1
:no_vs
echo ERROR: Could not find the Visual Studio 2022 C++ x64 build tools.
echo Install "Desktop development with C++" or the C++ Build Tools from the Visual Studio Installer.
exit /b 1
:no_ninja
echo ERROR: Ninja not found. Install it with: winget install Ninja-build.Ninja
exit /b 1
:no_mysql
echo ERROR: MariaDB client headers not found in %MYSQL_ROOT%
echo They come with the prepared Creator runtime, see etc\creator-test\PACKAGING.md.
exit /b 1
:failed
echo.
echo BUILD FAILED. See the messages above.
exit /b 1
