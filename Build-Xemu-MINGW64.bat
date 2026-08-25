@echo off
setlocal EnableExtensions EnableDelayedExpansion

rem ============================================================================
rem  Xemu Native MSYS2 / MINGW64 Clean Builder v1.08
rem  Uses existing MSYS2/MINGW64. Installs NOTHING.
rem  Builds from a disposable copy and preserves fresh failure logs.
rem ============================================================================

set "SCRIPT_DIR=%~dp0"
if not defined MSYS2_ROOT set "MSYS2_ROOT=C:\msys64"
if not defined XEMU_JOBS set "XEMU_JOBS=12"
if not defined XEMU_KEEP_TEMP set "XEMU_KEEP_TEMP=0"

if not "%~1"=="" (
    set "SOURCE_ROOT=%~f1"
) else (
    set "SOURCE_ROOT=%SCRIPT_DIR%"
    if not exist "%SCRIPT_DIR%build.sh" (
        if exist "%SCRIPT_DIR%Xemu\build.sh" set "SOURCE_ROOT=%SCRIPT_DIR%Xemu"
    )
)
pushd "%SOURCE_ROOT%" >nul 2>&1
if errorlevel 1 goto :source_failed
set "SOURCE_ROOT=%CD%"
popd
if not exist "%SOURCE_ROOT%\build.sh" goto :source_failed

if exist "%MSYS2_ROOT%\msys2_shell.cmd" goto :msys2_found
for %%D in (C D E F G) do (
    if exist "%%D:\msys64\msys2_shell.cmd" (
        set "MSYS2_ROOT=%%D:\msys64"
        goto :msys2_found
    )
)
echo [ERROR] MSYS2 not found.
goto :fail_early

:msys2_found
for %%I in ("%SOURCE_ROOT%\..") do set "SOURCE_PARENT=%%~fI"
set "OUTPUT_ROOT=%SOURCE_PARENT%\Xemu-MINGW64-Output"
set "RUN_ID=%RANDOM%%RANDOM%%RANDOM%"
set "WORK_ROOT=%TEMP%\Xemu-MINGW64-Build-%RUN_ID%"
set "WORK_SOURCE=%WORK_ROOT%\Xemu"
set "COPY_LOG=%TEMP%\Xemu-MINGW64-Copy-%RUN_ID%.log"
set "MSYS_SCRIPT_WIN=%MSYS2_ROOT%\tmp\xemu-native-build-%RUN_ID%.sh"
set "MSYS_SCRIPT_B64=%TEMP%\xemu-native-build-%RUN_ID%.b64"
set "MSYS_SCRIPT_UNIX=/tmp/xemu-native-build-%RUN_ID%.sh"
set "FAIL_ROOT=%OUTPUT_ROOT%\FAILED-%RUN_ID%"

echo.
echo ============================================================================
echo  Xemu Native MINGW64 Clean Build v1.08
echo ============================================================================
echo  Run ID : %RUN_ID%
echo  Source : %SOURCE_ROOT%
echo  MSYS2  : %MSYS2_ROOT%
echo  Jobs   : %XEMU_JOBS%
echo  Temp   : %WORK_SOURCE%
echo  Output : %OUTPUT_ROOT%
echo ============================================================================
echo.

if exist "%WORK_ROOT%" rmdir /s /q "%WORK_ROOT%"
mkdir "%WORK_SOURCE%" >nul 2>&1
echo [1/6] Copying source to temporary build tree...
robocopy "%SOURCE_ROOT%" "%WORK_SOURCE%" /E /COPY:DAT /DCOPY:DAT /R:2 /W:1 /XJ /XD ".git" "build" "dist" "__pycache__" /XF "build.log" "*.pyc" "*.pyo" > "%COPY_LOG%"
set "COPY_RC=!ERRORLEVEL!"
if !COPY_RC! GEQ 8 goto :build_failed

where certutil >nul 2>&1
if errorlevel 1 goto :build_failed
>"%MSYS_SCRIPT_B64%" (
    echo IyEvdXNyL2Jpbi9lbnYgYmFzaApzZXQgLWV1byBwaXBlZmFpbAoKZXhwb3J0IFRBUl9PUFRJT05T
    echo PSItLWZvcmNlLWxvY2FsJHtUQVJfT1BUSU9OUzorICRUQVJfT1BUSU9OU30iCgpTT1VSQ0U9IiQo
    echo Y3lncGF0aCAtdSAiJHtYRU1VX1RFTVBfU09VUkNFX1dJTjo/WEVNVV9URU1QX1NPVVJDRV9XSU4g
    echo aXMgbm90IHNldH0iKSIKSk9CUz0iJHtYRU1VX0pPQlM6LTEyfSIKY2QgIiRTT1VSQ0UiCgpMT0df
    echo RElSPSIkU09VUkNFLy54ZW11LW5hdGl2ZS1idWlsZC1sb2dzIgpta2RpciAtcCAiJExPR19ESVIi
    echo CgplY2hvICJbMi82XSBWYWxpZGF0aW5nIE1TWVMyL01JTkdXNjQgZW52aXJvbm1lbnQuLi4iCmlm
    echo IFtbICIke01TWVNURU06LX0iICE9ICJNSU5HVzY0IiBdXTsgdGhlbgogICAgcHJpbnRmICJFUlJP
    echo UjogRXhwZWN0ZWQgTVNZU1RFTT1NSU5HVzY0LCBnb3QgPCVxPlxuIiAiJHtNU1lTVEVNOi19Igog
    echo ICAgZXhpdCAyMApmaQoKR0NDX1BBVEg9IiQoY29tbWFuZCAtdiBnY2MgfHwgdHJ1ZSkiCmNhc2Ug
    echo IiRHQ0NfUEFUSCIgaW4KICAgIC9taW5ndzY0L2Jpbi9nY2MqKSA7OwogICAgKikKICAgICAgICBl
    echo Y2hvICJFUlJPUjogTUlOR1c2NCBpcyBhY3RpdmUsIGJ1dCBnY2MgZGlkIG5vdCByZXNvbHZlIGZy
    echo b20gL21pbmd3NjQvYmluOiAkR0NDX1BBVEgiCiAgICAgICAgZXhpdCAyMAogICAgICAgIDs7CmVz
    echo YWMKCnsKICAgIGVjaG8gIk1TWVNURU09JHtNU1lTVEVNOi19IgogICAgZWNobyAiTUlOR1dfUFJF
    echo RklYPSR7TUlOR1dfUFJFRklYOi19IgogICAgZWNobyAiU09VUkNFPSRTT1VSQ0UiCiAgICBlY2hv
    echo ICJKT0JTPSRKT0JTIgogICAgZWNobyAiZ2NjPSQoJEdDQ19QQVRIIC1kdW1wZnVsbHZlcnNpb24g
    echo LWR1bXB2ZXJzaW9uKSIKICAgIGVjaG8gImcrKz0kKGcrKyAtZHVtcGZ1bGx2ZXJzaW9uIC1kdW1w
    echo dmVyc2lvbikiCiAgICBlY2hvICJweXRob249JChweXRob24zIC0tdmVyc2lvbiAyPiYxKSIKICAg
    echo IGVjaG8gIlRBUl9PUFRJT05TPSR7VEFSX09QVElPTlM6LX0iCn0gPiAiJExPR19ESVIvZW52aXJv
    echo bm1lbnQudHh0IgpjYXQgIiRMT0dfRElSL2Vudmlyb25tZW50LnR4dCIKCm1pc3NpbmdfdG9vbHM9
    echo MAo6ID4gIiRMT0dfRElSL2RlcGVuZGVuY3ktY2hlY2sudHh0Igpmb3IgdG9vbCBpbiBnY2MgZysr
    echo IHB5dGhvbjMgcGtnLWNvbmZpZyBuaW5qYSBjbWFrZSBtYWtlIGN1cmwgdGFyOyBkbwogICAgaWYg
    echo dG9vbF9wYXRoPSIkKGNvbW1hbmQgLXYgIiR0b29sIiAyPi9kZXYvbnVsbCkiOyB0aGVuCiAgICAg
    echo ICAgZWNobyAiRk9VTkQgVE9PTDogJHRvb2wgLT4gJHRvb2xfcGF0aCIgfCB0ZWUgLWEgIiRMT0df
    echo RElSL2RlcGVuZGVuY3ktY2hlY2sudHh0IgogICAgZWxzZQogICAgICAgIGVjaG8gIk1JU1NJTkcg
    echo VE9PTDogJHRvb2wiIHwgdGVlIC1hICIkTE9HX0RJUi9kZXBlbmRlbmN5LWNoZWNrLnR4dCIKICAg
    echo ICAgICBtaXNzaW5nX3Rvb2xzPTEKICAgIGZpCmRvbmUKaWYgW1sgIiRtaXNzaW5nX3Rvb2xzIiAt
    echo bmUgMCBdXTsgdGhlbgogICAgZWNobyAiRVJST1I6IE9uZSBvciBtb3JlIHJlcXVpcmVkIGJ1aWxk
    echo IHRvb2xzIGFyZSBtaXNzaW5nLiIKICAgIGV4aXQgMjEKZmkKCmVjaG8gIi0tLSBwa2ctY29uZmln
    echo IHZlcnNpb25zIC0tLSIgfCB0ZWUgLWEgIiRMT0dfRElSL2RlcGVuZGVuY3ktY2hlY2sudHh0Igpm
    echo b3IgZGVwIGluIGNhcHN0b25lIGdsaWItMi4wIGVwb3h5IHNkbDIgcGl4bWFuLTEgbGlidXNiLTEu
    echo MCBzYW1wbGVyYXRlIHNsaXJwIGxpYmN1cmwgb3BlbnNzbDsgZG8KICAgIGlmIHBrZy1jb25maWcg
    echo LS1leGlzdHMgIiRkZXAiOyB0aGVuCiAgICAgICAgZWNobyAiJGRlcD0kKHBrZy1jb25maWcgLS1t
    echo b2R2ZXJzaW9uICIkZGVwIikiIHwgdGVlIC1hICIkTE9HX0RJUi9kZXBlbmRlbmN5LWNoZWNrLnR4
    echo dCIKICAgIGVsc2UKICAgICAgICBlY2hvICIkZGVwPW5vdC1mb3VuZC1vci1ub3QtcmVxdWlyZWQi
    echo IHwgdGVlIC1hICIkTE9HX0RJUi9kZXBlbmRlbmN5LWNoZWNrLnR4dCIKICAgIGZpCmRvbmUKCmVj
    echo aG8gIlsyYi82XSBWZXJpZnlpbmcgR05VIHRhciBkcml2ZS1sZXR0ZXIgaGFuZGxpbmcuLi4iClRB
    echo Ul9URVNUX0RJUj0iJExPR19ESVIvdGFyLXByZWZsaWdodCIKcm0gLXJmICIkVEFSX1RFU1RfRElS
    echo Igpta2RpciAtcCAiJFRBUl9URVNUX0RJUi9zcmMiICIkVEFSX1RFU1RfRElSL291dCIKcHJpbnRm
    echo ICJ4ZW11LXRhci1wcmVmbGlnaHRcbiIgPiAiJFRBUl9URVNUX0RJUi9zcmMvcGF5bG9hZC50eHQi
    echo Ci91c3IvYmluL3RhciBjemYgIiRUQVJfVEVTVF9ESVIvdGVzdC50YXIuZ3oiIC1DICIkVEFSX1RF
    echo U1RfRElSL3NyYyIgcGF5bG9hZC50eHQKVEFSX1RFU1RfQVJDSElWRT0iJChjeWdwYXRoIC1tICIk
    echo VEFSX1RFU1RfRElSL3Rlc3QudGFyLmd6IikiClRBUl9URVNUX09VVFBVVD0iJChjeWdwYXRoIC1t
    echo ICIkVEFSX1RFU1RfRElSL291dCIpIgppZiAhIC91c3IvYmluL3RhciB4emYgIiRUQVJfVEVTVF9B
    echo UkNISVZFIiAtQyAiJFRBUl9URVNUX09VVFBVVCIgPiIkTE9HX0RJUi90YXItcHJlZmxpZ2h0LnR4
    echo dCIgMj4mMTsgdGhlbgogICAgZWNobyAiRVJST1I6IEdOVSB0YXIgc3RpbGwgY2Fubm90IGV4dHJh
    echo Y3QgYSBuYXRpdmUgZHJpdmUtbGV0dGVyIHBhdGguIgogICAgY2F0ICIkTE9HX0RJUi90YXItcHJl
    echo ZmxpZ2h0LnR4dCIKICAgIGV4aXQgMjMKZmkKaWYgW1sgISAtZiAiJFRBUl9URVNUX0RJUi9vdXQv
    echo cGF5bG9hZC50eHQiIF1dOyB0aGVuCiAgICBlY2hvICJFUlJPUjogR05VIHRhciBwcmVmbGlnaHQg
    echo ZGlkIG5vdCBwcm9kdWNlIHRoZSBleHBlY3RlZCBmaWxlLiIKICAgIGV4aXQgMjMKZmkKZWNobyAi
    echo R05VIHRhciBkcml2ZS1sZXR0ZXIgcHJlZmxpZ2h0OiBQQVNTIiB8IHRlZSAiJExPR19ESVIvdGFy
    echo LXByZWZsaWdodC50eHQiCnJtIC1yZiAiJFRBUl9URVNUX0RJUiIKCmVjaG8gIlsyYy82XSBWZXJp
    echo ZnlpbmcgV2luZG93cyBzeW1ib2xpYy1saW5rIHByaXZpbGVnZS4uLiIKU1lNTElOS19URVNUX0RJ
    echo Uj0iJExPR19ESVIvc3ltbGluay1wcmVmbGlnaHQiCnJtIC1yZiAiJFNZTUxJTktfVEVTVF9ESVIi
    echo Cm1rZGlyIC1wICIkU1lNTElOS19URVNUX0RJUiIKcHJpbnRmICJ4ZW11LXN5bWxpbmstcHJlZmxp
    echo Z2h0XG4iID4gIiRTWU1MSU5LX1RFU1RfRElSL3NvdXJjZS50eHQiCnNldCArZQpweXRob24zIC0g
    echo IiRTWU1MSU5LX1RFU1RfRElSIiA+IiRMT0dfRElSL3N5bWxpbmstcHJlZmxpZ2h0LnR4dCIgMj4m
    echo MSA8PCdQWScKaW1wb3J0IG9zCmltcG9ydCBzeXMKZnJvbSBwYXRobGliIGltcG9ydCBQYXRoCgpy
    echo b290ID0gUGF0aChzeXMuYXJndlsxXSkKc3JjID0gcm9vdCAvICJzb3VyY2UudHh0Igpkc3QgPSBy
    echo b290IC8gImxpbmsudHh0IgoKdHJ5OgogICAgb3Muc3ltbGluayhzcmMsIGRzdCkKZXhjZXB0IE9T
    echo RXJyb3IgYXMgZXhjOgogICAgcHJpbnQoZiJFUlJPUjogUHl0aG9uIG9zLnN5bWxpbmsoKSBmYWls
    echo ZWQ6IHtleGN9IikKICAgIHN5cy5leGl0KDEpCgppZiBub3QgZHN0LmlzX3N5bWxpbmsoKToKICAg
    echo IHByaW50KCJFUlJPUjogb3Muc3ltbGluaygpIHJldHVybmVkIHdpdGhvdXQgY3JlYXRpbmcgYSBz
    echo eW1ib2xpYyBsaW5rLiIpCiAgICBzeXMuZXhpdCgxKQoKcHJpbnQoIldpbmRvd3MvUHl0aG9uIHN5
    echo bWJvbGljLWxpbmsgcHJlZmxpZ2h0OiBQQVNTIikKUFkKc3ltbGlua19yYz0kPwpzZXQgLWUKaWYg
    echo W1sgIiRzeW1saW5rX3JjIiAtbmUgMCBdXTsgdGhlbgogICAgY2F0ICIkTE9HX0RJUi9zeW1saW5r
    echo LXByZWZsaWdodC50eHQiCiAgICBlY2hvCiAgICBlY2hvICJFUlJPUjogV2luZG93cyBzeW1ib2xp
    echo Yy1saW5rIGNyZWF0aW9uIGlzIG5vdCBwZXJtaXR0ZWQgZm9yIHRoaXMgcHJvY2Vzcy4iCiAgICBl
    echo Y2hvICJFbmFibGUgV2luZG93cyBEZXZlbG9wZXIgTW9kZSwgdGhlbiByZXJ1biB0aGlzIEJBVC4i
    echo CiAgICBlY2hvICJBbHRlcm5hdGl2ZTogcnVuIHRoZSBCQVQgYXMgQWRtaW5pc3RyYXRvci4iCiAg
    echo ICBlY2hvICJUaGlzIGlzIHJlcXVpcmVkIGJ5IHhlbXUncyBzY3JpcHRzL3N5bWxpbmstaW5zdGFs
    echo bC10cmVlLnB5LiIKICAgIGV4aXQgMjQKZmkKY2F0ICIkTE9HX0RJUi9zeW1saW5rLXByZWZsaWdo
    echo dC50eHQiCnJtIC1yZiAiJFNZTUxJTktfVEVTVF9ESVIiCgplY2hvICJbMy82XSBSdW5uaW5nIERl
    echo YnVnIFRvb2xzIHByb2plY3QtbGF5b3V0IHZhbGlkYXRpb24uLi4iCnNldCArZQpweXRob24zIC4v
    echo dWkveHVpL2RlYnVnLXRvb2xzL3ZhbGlkYXRlLXByb2plY3QtbGF5b3V0LnB5IC0tcm9vdCAuIDI+
    echo JjEgfCB0ZWUgIiRMT0dfRElSL2xheW91dC12YWxpZGF0aW9uLnR4dCIKbGF5b3V0X3JjPSR7UElQ
    echo RVNUQVRVU1swXX0Kc2V0IC1lCmlmIFtbICIkbGF5b3V0X3JjIiAtbmUgMCBdXTsgdGhlbgogICAg
    echo ZWNobyAiRVJST1I6IERlYnVnIFRvb2xzIGxheW91dCB2YWxpZGF0aW9uIGZhaWxlZCB3aXRoIGNv
    echo ZGUgJGxheW91dF9yYy4iCiAgICBleGl0ICIkbGF5b3V0X3JjIgpmaQoKZWNobyAiWzQvNl0gUmVt
    echo b3ZpbmcgY29waWVkIGJ1aWxkIG91dHB1dHMuLi4iCnJtIC1yZiBidWlsZCBkaXN0CgplY2hvICJb
    echo NS82XSBCdWlsZGluZyB4ZW11IG5hdGl2ZWx5IHdpdGggTUlOR1c2NC4uLiIKc2V0ICtlCmJhc2gg
    echo Li9idWlsZC5zaCAtaiIkSk9CUyIgLS1lbmFibGUtY2Fwc3RvbmUgMj4mMSB8IHRlZSBidWlsZC5s
    echo b2cKYnVpbGRfcmM9JHtQSVBFU1RBVFVTWzBdfQpzZXQgLWUKaWYgW1sgIiRidWlsZF9yYyIgLW5l
    echo IDAgXV07IHRoZW4KICAgIGVjaG8gIkVSUk9SOiB4ZW11IGJ1aWxkIGZhaWxlZCB3aXRoIGNvZGUg
    echo JGJ1aWxkX3JjLiIKICAgIGV4aXQgIiRidWlsZF9yYyIKZmkKCmlmIFtbICEgLWYgZGlzdC94ZW11
    echo LmV4ZSBdXTsgdGhlbgogICAgZWNobyAiRVJST1I6IEJ1aWxkIGZpbmlzaGVkIHdpdGhvdXQgZGlz
    echo dC94ZW11LmV4ZSIKICAgIGV4aXQgMjIKZmkKCmVjaG8gIk5hdGl2ZSBNSU5HVzY0IGJ1aWxkIGNv
    echo bXBsZXRlZCBzdWNjZXNzZnVsbHkuIgo=
)
certutil -f -decode "%MSYS_SCRIPT_B64%" "%MSYS_SCRIPT_WIN%" >nul 2>&1
if errorlevel 1 goto :build_failed
del /q "%MSYS_SCRIPT_B64%" >nul 2>&1
set "XEMU_TEMP_SOURCE_WIN=%WORK_SOURCE%"
echo [2/6] Starting your existing MSYS2 MINGW64 environment...
call "%MSYS2_ROOT%\msys2_shell.cmd" -defterm -no-start -mingw64 -c "bash %MSYS_SCRIPT_UNIX%"
set "BUILD_RC=%ERRORLEVEL%"
del /q "%MSYS_SCRIPT_WIN%" >nul 2>&1
if not "%BUILD_RC%"=="0" goto :build_failed

echo [6/6] Copying packaged output and logs...
if exist "%OUTPUT_ROOT%\dist" rmdir /s /q "%OUTPUT_ROOT%\dist"
if exist "%OUTPUT_ROOT%\logs" rmdir /s /q "%OUTPUT_ROOT%\logs"
mkdir "%OUTPUT_ROOT%\dist" >nul 2>&1
mkdir "%OUTPUT_ROOT%\logs" >nul 2>&1
robocopy "%WORK_SOURCE%\dist" "%OUTPUT_ROOT%\dist" /MIR /COPY:DAT /DCOPY:DAT /R:2 /W:1 >nul
if errorlevel 8 goto :output_copy_failed
if exist "%WORK_SOURCE%\build.log" copy /y "%WORK_SOURCE%\build.log" "%OUTPUT_ROOT%\logs\build.log" >nul
if exist "%WORK_SOURCE%\.xemu-native-build-logs" robocopy "%WORK_SOURCE%\.xemu-native-build-logs" "%OUTPUT_ROOT%\logs" /E /COPY:DAT /R:2 /W:1 >nul
if exist "%WORK_SOURCE%\build\meson-logs\meson-log.txt" copy /y "%WORK_SOURCE%\build\meson-logs\meson-log.txt" "%OUTPUT_ROOT%\logs\meson-log.txt" >nul
if exist "%WORK_SOURCE%\build\meson-logs\meson-setup.txt" copy /y "%WORK_SOURCE%\build\meson-logs\meson-setup.txt" "%OUTPUT_ROOT%\logs\meson-setup.txt" >nul
>"%OUTPUT_ROOT%\BUILD-INFO.txt" echo Builder: v1.08
>>"%OUTPUT_ROOT%\BUILD-INFO.txt" echo Run ID: %RUN_ID%
>>"%OUTPUT_ROOT%\BUILD-INFO.txt" echo Source: %SOURCE_ROOT%
>>"%OUTPUT_ROOT%\BUILD-INFO.txt" echo Built: %DATE% %TIME%
if exist "%COPY_LOG%" del /q "%COPY_LOG%" >nul 2>&1
echo.
echo ============================================================================
echo  BUILD SUCCESSFUL
echo ============================================================================
echo  Executable: %OUTPUT_ROOT%\dist\xemu.exe
echo  Logs      : %OUTPUT_ROOT%\logs
echo ============================================================================
if "%XEMU_KEEP_TEMP%"=="1" (
    echo Temporary tree retained: %WORK_SOURCE%
) else (
    rmdir /s /q "%WORK_ROOT%" >nul 2>&1
)
if not defined XEMU_NO_PAUSE pause
exit /b 0

:output_copy_failed
echo [ERROR] Build succeeded but output copy failed.
goto :build_failed

:build_failed
if not defined BUILD_RC set "BUILD_RC=1"
if exist "%MSYS_SCRIPT_B64%" del /q "%MSYS_SCRIPT_B64%" >nul 2>&1
if exist "%MSYS_SCRIPT_WIN%" del /q "%MSYS_SCRIPT_WIN%" >nul 2>&1
if exist "%FAIL_ROOT%" rmdir /s /q "%FAIL_ROOT%"
mkdir "%FAIL_ROOT%" >nul 2>&1
if exist "%COPY_LOG%" copy /y "%COPY_LOG%" "%FAIL_ROOT%\robocopy.log" >nul
if exist "%WORK_SOURCE%\build.log" copy /y "%WORK_SOURCE%\build.log" "%FAIL_ROOT%\build.log" >nul
if exist "%WORK_SOURCE%\.xemu-native-build-logs" robocopy "%WORK_SOURCE%\.xemu-native-build-logs" "%FAIL_ROOT%" /E /COPY:DAT /R:2 /W:1 >nul
if exist "%WORK_SOURCE%\build\meson-logs\meson-log.txt" copy /y "%WORK_SOURCE%\build\meson-logs\meson-log.txt" "%FAIL_ROOT%\meson-log.txt" >nul
if exist "%WORK_SOURCE%\build\meson-logs\meson-setup.txt" copy /y "%WORK_SOURCE%\build\meson-logs\meson-setup.txt" "%FAIL_ROOT%\meson-setup.txt" >nul
>"%FAIL_ROOT%\FAILED-BUILD-INFO.txt" echo Builder: v1.08
>>"%FAIL_ROOT%\FAILED-BUILD-INFO.txt" echo Run ID: %RUN_ID%
>>"%FAIL_ROOT%\FAILED-BUILD-INFO.txt" echo Source: %SOURCE_ROOT%
>>"%FAIL_ROOT%\FAILED-BUILD-INFO.txt" echo Temp: %WORK_SOURCE%
>>"%FAIL_ROOT%\FAILED-BUILD-INFO.txt" echo Exit code: %BUILD_RC%
>>"%FAIL_ROOT%\FAILED-BUILD-INFO.txt" echo Failed: %DATE% %TIME%
echo.
echo ============================================================================
echo  BUILD FAILED
echo ============================================================================
echo  Run ID: %RUN_ID%
echo  Fresh logs: %FAIL_ROOT%
echo  Temp tree : %WORK_SOURCE%
echo ============================================================================
if not defined XEMU_NO_PAUSE pause
exit /b 1

:source_failed
echo [ERROR] xemu source directory/build.sh not found.
:fail_early
if not defined XEMU_NO_PAUSE pause
exit /b 1
