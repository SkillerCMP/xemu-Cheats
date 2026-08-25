@echo off
setlocal EnableExtensions EnableDelayedExpansion

rem ============================================================================
rem  Xemu GitHub-Equivalent Windows Release Builder v1.06
rem  Mirrors .github/workflows/build.yml + build-windows.yml (x86_64/release)
rem  Uses the same pinned GHCR GCC/MXE toolchain and cv2pdb 0.52 post-process.
rem ============================================================================

set "TOOLCHAIN_IMAGE=ghcr.io/xemu-project/xemu-win64-toolchain-gcc:sha-2881edd"
set "CV2PDB_URL=https://github.com/rainers/cv2pdb/releases/download/v0.52/cv2pdb-0.52.zip"
set "SCRIPT_DIR=%~dp0"
if not "%~1"=="" (
  set "SOURCE_ROOT=%~f1"
) else (
  set "SOURCE_ROOT=%SCRIPT_DIR%"
)
pushd "%SOURCE_ROOT%" >nul 2>&1 || goto :source_failed
set "SOURCE_ROOT=%CD%"
popd
if not exist "%SOURCE_ROOT%\build.sh" goto :source_failed
if not exist "%SOURCE_ROOT%\.github\workflows\build-windows.yml" goto :source_failed

call :select_debug_tools_profile "%~2"
if errorlevel 1 goto :fail_early

where docker >nul 2>&1 || goto :docker_missing
docker info >nul 2>&1 || goto :docker_not_running

for %%I in ("%SOURCE_ROOT%\..") do set "SOURCE_PARENT=%%~fI"
set "RUN_ID=%RANDOM%%RANDOM%%RANDOM%"
set "OUTPUT_BASE=%SOURCE_PARENT%\Xemu-GitHub-Windows-Output"
set "RUN_ROOT=%OUTPUT_BASE%\RUN-%RUN_ID%"
set "DIST_ROOT=%RUN_ROOT%\dist"
set "LOG_ROOT=%RUN_ROOT%\logs"

if exist "%RUN_ROOT%" rmdir /s /q "%RUN_ROOT%"
mkdir "%RUN_ROOT%" >nul 2>&1
mkdir "%LOG_ROOT%" >nul 2>&1
>"%RUN_ROOT%\DEBUG_TOOLS_PROFILE.txt" echo %DEBUG_TOOLS_PROFILE%

echo.
echo ============================================================================
echo  Xemu GitHub-Equivalent Windows Release Build v1.06
echo ============================================================================
echo  Source    : %SOURCE_ROOT%
echo  Output    : %RUN_ROOT%
echo  Toolchain : %TOOLCHAIN_IMAGE%
echo  Build     : x86_64 release, static, LTO, x86_version=3
echo  DebugTools: %DEBUG_TOOLS_PROFILE%
echo  Symbols   : cv2pdb 0.52, matching GitHub post-processing
echo  Dependencies: Debug Tools owns Capstone + Keystone bootstrap
echo ============================================================================
echo.

echo [1/7] Pulling the pinned GitHub Windows toolchain...
docker pull "%TOOLCHAIN_IMAGE%"
if errorlevel 1 goto :failed

echo [2/7] Using Debug Tools-owned Docker build driver...
if not exist "%SOURCE_ROOT%\ui\xui\debug-tools\docker-build-windows.sh" goto :source_failed

echo [3/7] Running GitHub-equivalent source validation and Windows cross build...
docker run --rm --platform linux/amd64 ^
  --env "XEMU_DEBUG_TOOLS_PROFILE=%DEBUG_TOOLS_PROFILE%" ^
  --mount "type=bind,source=%SOURCE_ROOT%,target=/input,readonly" ^
  --mount "type=bind,source=%RUN_ROOT%,target=/output" ^
  "%TOOLCHAIN_IMAGE%" bash /input/ui/xui/debug-tools/docker-build-windows.sh
if errorlevel 1 goto :failed
if not exist "%DIST_ROOT%\xemu.exe" goto :failed

set "PKG_VERSION=unknown"
if exist "%RUN_ROOT%\PACKAGE_VERSION.txt" set /p PKG_VERSION=<"%RUN_ROOT%\PACKAGE_VERSION.txt"

echo [4/7] Preparing cv2pdb 0.52 exactly like GitHub...
rem GitHub downloads cv2pdb fresh into the Windows job and extracts it to a
rem known directory. Do the same here. Do not use recursive cache discovery:
rem an invalid/stale cached path can make CMD fail before cv2pdb even starts.
set "CV2PDB_ZIP=%RUN_ROOT%\cv2pdb.zip"
set "CV2PDB_ROOT=%RUN_ROOT%\cv2pdb"
set "CV2PDB_EXE=%CV2PDB_ROOT%\cv2pdb64.exe"
if exist "%CV2PDB_ROOT%" rmdir /s /q "%CV2PDB_ROOT%"
if exist "%CV2PDB_ZIP%" del /q "%CV2PDB_ZIP%"

powershell -NoProfile -ExecutionPolicy Bypass -Command "Invoke-WebRequest -UseBasicParsing -Uri '%CV2PDB_URL%' -OutFile '%CV2PDB_ZIP%'"
if errorlevel 1 goto :failed
powershell -NoProfile -ExecutionPolicy Bypass -Command "Expand-Archive -LiteralPath '%CV2PDB_ZIP%' -DestinationPath '%CV2PDB_ROOT%' -Force"
if errorlevel 1 goto :failed

dir /s /b "%CV2PDB_ROOT%" > "%LOG_ROOT%\cv2pdb-extract.txt" 2>&1
if not exist "%CV2PDB_EXE%" (
  echo [ERROR] Expected GitHub cv2pdb executable was not found:
  echo         %CV2PDB_EXE%
  echo [ERROR] Extracted-file listing: %LOG_ROOT%\cv2pdb-extract.txt
  goto :failed
)

rem Record the converter itself so a local run can be audited against GitHub.
certutil -hashfile "%CV2PDB_EXE%" SHA256 > "%LOG_ROOT%\cv2pdb64.sha256.txt"

echo [5/7] Generating PDB and stripping xemu.exe exactly like GitHub...
pushd "%DIST_ROOT%" >nul || goto :failed
"%CV2PDB_EXE%" xemu.exe > "%LOG_ROOT%\cv2pdb.txt" 2>&1
set "CV2PDB_RC=!ERRORLEVEL!"
popd
if not "!CV2PDB_RC!"=="0" goto :failed
if not exist "%DIST_ROOT%\xemu.pdb" goto :failed

for %%F in ("%DIST_ROOT%\xemu.exe") do set "EXE_SIZE=%%~zF"
for %%F in ("%DIST_ROOT%\xemu.pdb") do set "PDB_SIZE=%%~zF"
certutil -hashfile "%DIST_ROOT%\xemu.exe" SHA256 > "%LOG_ROOT%\xemu-post-cv2pdb.sha256.txt"
certutil -hashfile "%DIST_ROOT%\xemu.pdb" SHA256 > "%LOG_ROOT%\xemu-pdb.sha256.txt"

echo [6/7] Packaging release and symbols archives...
set "SEVENZIP="
for %%I in (7z.exe) do set "SEVENZIP=%%~$PATH:I"
if not defined SEVENZIP if exist "%ProgramFiles%\7-Zip\7z.exe" set "SEVENZIP=%ProgramFiles%\7-Zip\7z.exe"
if not defined SEVENZIP if exist "C:\Program Files (x86)\7-Zip\7z.exe" set "SEVENZIP=C:\Program Files (x86)\7-Zip\7z.exe"
set "PACKAGE_ROOT=%RUN_ROOT%\dist-pdb"
mkdir "%PACKAGE_ROOT%" >nul 2>&1
rem 7-Zip uses ! in its include/exclude wildcard switches. The BAT normally
rem runs with delayed expansion enabled, which would consume those ! characters
rem and turn -xr!*.pdb / -ir!*.pdb into invalid 7-Zip switches.
setlocal DisableDelayedExpansion
if defined SEVENZIP (
  pushd "%DIST_ROOT%" >nul
  "%SEVENZIP%" a -tzip "%PACKAGE_ROOT%\xemu-%PKG_VERSION%-windows-x86_64.zip" * "-xr!*.pdb" > "%LOG_ROOT%\package-release.txt" 2>&1
  if errorlevel 1 (popd & endlocal & goto :failed)
  "%SEVENZIP%" a -tzip "%PACKAGE_ROOT%\xemu-%PKG_VERSION%-windows-x86_64-pdb.zip" "-ir!*.pdb" > "%LOG_ROOT%\package-pdb.txt" 2>&1
  if errorlevel 1 (popd & endlocal & goto :failed)
  popd
) else (
  echo [WARN] 7-Zip was not found. GitHub-style ZIP packaging was skipped.
  echo [WARN] xemu.exe and xemu.pdb are still complete in: %DIST_ROOT%
)
endlocal

echo [7/7] Final verification...
if not exist "%DIST_ROOT%\xemu.exe" goto :failed
if not exist "%DIST_ROOT%\xemu.pdb" goto :failed

echo.
echo ============================================================================
echo  GITHUB-EQUIVALENT WINDOWS BUILD COMPLETE
echo ============================================================================
echo  Version : %PKG_VERSION%
echo  Profile : %DEBUG_TOOLS_PROFILE%
echo  EXE     : %DIST_ROOT%\xemu.exe
echo  EXE size: !EXE_SIZE! bytes
echo  PDB     : %DIST_ROOT%\xemu.pdb
echo  PDB size: !PDB_SIZE! bytes
echo  Logs    : %LOG_ROOT%
echo  Source  : %RUN_ROOT%\source
if defined SEVENZIP echo  Packages: %PACKAGE_ROOT%
echo ============================================================================
echo.
if not defined XEMU_NO_PAUSE pause
exit /b 0

:select_debug_tools_profile
set "PROFILE_INPUT=%~1"
if not defined PROFILE_INPUT if defined XEMU_DEBUG_TOOLS_PROFILE set "PROFILE_INPUT=%XEMU_DEBUG_TOOLS_PROFILE%"
if defined PROFILE_INPUT goto :normalize_debug_tools_profile

echo.
echo Select Debug Tools build profile:
echo   [1] main        - Current Game + Cheat Engine only
echo   [2] main+hdd    - Main + HDD Directory / Kernel RPC
echo   [3] main+memory - Main + Memory Viewer / Search / x86 Debugger
echo   [4] full        - All Debug Tools additions
echo.
choice /c 1234 /n /m "Select profile [1-4]: "
if errorlevel 4 (
  set "PROFILE_INPUT=full"
  goto :debug_tools_profile_selected
)
if errorlevel 3 (
  set "PROFILE_INPUT=main+memory"
  goto :debug_tools_profile_selected
)
if errorlevel 2 (
  set "PROFILE_INPUT=main+hdd"
  goto :debug_tools_profile_selected
)
if errorlevel 1 (
  set "PROFILE_INPUT=main"
  goto :debug_tools_profile_selected
)
echo [ERROR] No Debug Tools profile was selected.
exit /b 1

:normalize_debug_tools_profile
if /i "%PROFILE_INPUT%"=="1" set "PROFILE_INPUT=main"
if /i "%PROFILE_INPUT%"=="2" set "PROFILE_INPUT=main+hdd"
if /i "%PROFILE_INPUT%"=="3" set "PROFILE_INPUT=main+memory"
if /i "%PROFILE_INPUT%"=="4" set "PROFILE_INPUT=full"
if /i "%PROFILE_INPUT%"=="main" goto :debug_tools_profile_selected
if /i "%PROFILE_INPUT%"=="main+hdd" goto :debug_tools_profile_selected
if /i "%PROFILE_INPUT%"=="main+memory" goto :debug_tools_profile_selected
if /i "%PROFILE_INPUT%"=="full" goto :debug_tools_profile_selected
echo [ERROR] Invalid Debug Tools profile: %PROFILE_INPUT%
echo [ERROR] Expected main, main+hdd, main+memory, or full.
exit /b 1

:debug_tools_profile_selected
set "DEBUG_TOOLS_PROFILE=%PROFILE_INPUT%"
exit /b 0

:docker_missing
echo [ERROR] Docker was not found in PATH.
echo Install/start Docker Desktop, then rerun this BAT.
goto :fail_early

:docker_not_running
echo [ERROR] Docker is installed but the Docker engine is not running.
echo Start Docker Desktop, then rerun this BAT.
goto :fail_early

:source_failed
echo [ERROR] xemu source root was not found.
echo Place this BAT in the xemu source root or pass the source folder as argument 1.
goto :fail_early

:failed
echo.
echo ============================================================================
echo  BUILD FAILED
echo ============================================================================
echo  Output/logs retained at: %RUN_ROOT%
echo ============================================================================
if not defined XEMU_NO_PAUSE pause
exit /b 1

:fail_early
if not defined XEMU_NO_PAUSE pause
exit /b 1
