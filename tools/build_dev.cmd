@echo off
rem Dev build helper. Loads the MSVC environment, then builds the given targets (default: all).
rem Usage: tools\build_dev.cmd [target ...]      e.g. tools\build_dev.cmd narration_tests
rem Needs Visual Studio and vcpkg installed, and build\ already configured (cmake --preset default).
rem Keep this file ASCII only: cmd reads it with the system code page.
setlocal
set "VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
  echo vcvars64.bat not found: %VCVARS%
  exit /b 1
)
call "%VCVARS%" >nul
cd /d "%~dp0.."
if "%~1"=="" (
  cmake --build build
) else (
  cmake --build build --target %*
)
exit /b %ERRORLEVEL%
