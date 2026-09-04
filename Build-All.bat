@echo off
setlocal
if "%~1"=="" (
  echo Usage: Build-All.bat ^<xemu-source.zip^> [version] [build-selection] [source] [debug-tools-profile]
  echo.
  echo Interactive example:
  echo   Build-All.bat xemu-master-Official.zip 3.07-local
  echo.
  echo Direct example:
  echo   Build-All.bat xemu-master-Official.zip 3.07-local Windows/x86_64/debug
  echo.
  echo Direct build plus patched source ZIP:
  echo   Build-All.bat xemu-master-Official.zip 3.07-local Windows/x86_64/debug source
  echo.
  echo Direct build with a Debug Tools profile:
  echo   Build-All.bat xemu-master-Official.zip 3.07-local Windows/x86_64/debug main+memory
  echo.
  echo Direct build, patched source ZIP, and Debug Tools profile:
  echo   Build-All.bat xemu-master-Official.zip 3.07-local Windows/x86_64/debug source full
  exit /b 2
)

set "VER=%~2"
if "%VER%"=="" set "VER=0.0.0-0-unofficial-local"
set "BUILD=%~3"
set "SOURCEOPT=%~4"
set "PROFILE=%~5"

rem A profile may be supplied as argument 4 when source export is not requested.
if /I "%SOURCEOPT%"=="main" (
  set "PROFILE=main"
  set "SOURCEOPT="
)
if /I "%SOURCEOPT%"=="main+hdd" (
  set "PROFILE=main+hdd"
  set "SOURCEOPT="
)
if /I "%SOURCEOPT%"=="main+memory" (
  set "PROFILE=main+memory"
  set "SOURCEOPT="
)
if /I "%SOURCEOPT%"=="full" (
  set "PROFILE=full"
  set "SOURCEOPT="
)

if not "%SOURCEOPT%"=="" if /I not "%SOURCEOPT%"=="source" (
  echo ERROR: Argument 4 must be source or a Debug Tools profile.
  exit /b 2
)

if "%BUILD%"=="" (
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Build-XemuMatrix.ps1" -SourceZip "%~1" -Version "%VER%" -InteractiveSelect
) else (
  if /I "%SOURCEOPT%"=="source" (
    if "%PROFILE%"=="" (
      powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Build-XemuMatrix.ps1" -SourceZip "%~1" -Version "%VER%" -BuildSelection "%BUILD%" -OutputPatchedSource
    ) else (
      powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Build-XemuMatrix.ps1" -SourceZip "%~1" -Version "%VER%" -BuildSelection "%BUILD%" -OutputPatchedSource -DebugToolsProfile "%PROFILE%"
    )
  ) else (
    if "%PROFILE%"=="" (
      powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Build-XemuMatrix.ps1" -SourceZip "%~1" -Version "%VER%" -BuildSelection "%BUILD%"
    ) else (
      powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Build-XemuMatrix.ps1" -SourceZip "%~1" -Version "%VER%" -BuildSelection "%BUILD%" -DebugToolsProfile "%PROFILE%"
    )
  )
)

set "RC=%ERRORLEVEL%"
endlocal & exit /b %RC%
