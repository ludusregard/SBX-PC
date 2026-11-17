@echo off
rem Build sbx with the native Vulkan renderer (the only renderer), for
rem Windows and then for Linux in WSL:
rem   build_vulkan.bat [preset]   (default win-amd64-relwithdebinfo)
rem
rem Windows: out\build\<preset>-vulkan\sbx.exe, logged to out\build_vulkan.log.
rem Linux: build_vulkan.sh run in WSL with the matching linux-amd64-* preset,
rem out\build\<linux preset>-vulkan\sbx, logged to out\build_vulkan_linux.log.
rem It runs only once the Windows build has succeeded, and needs WSL with
rem Ubuntu 24.04 and setup_linux.sh run once in it (docs\build_run.md).
rem   set SBX_NO_LINUX=1          skip the Linux build
rem   set SBX_WSL_DISTRO=<name>   the WSL distro to use (default: WSL's default)
rem
rem Configure runs every time. Logs are overwritten each run. On a failure
rem the last lines and every "error" line are shown and the window waits for
rem a key (set SBX_NOPAUSE=1 to skip that).
setlocal
cd /d "%~dp0"
set "PRESET=%~1"
if "%PRESET%"=="" set "PRESET=win-amd64-relwithdebinfo"
set "LPRESET=%PRESET:win-=linux-%"
set "B=out\build\%PRESET%-vulkan"
if not exist out mkdir out
set "LOG=%CD%\out\build_vulkan.log"
echo build_vulkan.bat %PRESET% - %DATE% %TIME% > "%LOG%"

if not exist "generated\default\sources.cmake" (
  echo No generated code yet: run codegen.bat first.
  echo No generated code yet: run codegen.bat first. >> "%LOG%"
  goto :failed
)

echo === Windows: configuring ^(%PRESET%, %B%^)
cmake --preset %PRESET% -B "%B%" >> "%LOG%" 2>&1 || goto :failed

echo === Windows: building ^(the first build compiles the Vulkan SDK and generated code: slow^)
cmake --build "%B%" >> "%LOG%" 2>&1 || goto :failed
if not exist "%B%\sbx.exe" (
  echo The build reported success but %B%\sbx.exe is missing. >> "%LOG%"
  goto :failed
)
echo.
echo Built %B%\sbx.exe  -  run it with run_vulkan.bat

rem ---- Linux, in WSL ----
if defined SBX_NO_LINUX (
  echo.
  echo Linux build skipped: SBX_NO_LINUX is set.
  goto :done
)
where wsl >nul 2>&1
if errorlevel 1 goto :no_wsl
set "WSL=wsl.exe"
if defined SBX_WSL_DISTRO set "WSL=wsl.exe -d %SBX_WSL_DISTRO%"
echo.
echo === Linux ^(WSL^): %LPRESET%
%WSL% --cd "%CD%" -e bash ./build_vulkan.sh %LPRESET%
if errorlevel 1 goto :linux_failed
echo.
echo Both builds done: %B%\sbx.exe and out\build\%LPRESET%-vulkan\sbx

:done
if not defined SBX_NOPAUSE pause
exit /b 0

:no_wsl
echo.
echo ===== Linux build skipped: WSL isn't installed.
echo Install it with: wsl --install -d Ubuntu-24.04
echo then run ./setup_linux.sh once from sbx_pc inside it.
echo Set SBX_NO_LINUX=1 to build Windows only without this notice.
if not defined SBX_NOPAUSE pause
exit /b 0

:linux_failed
echo.
echo ===== Linux build FAILED ^(the Windows build is fine^).
echo Log: %CD%\out\build_vulkan_linux.log
echo If WSL itself didn't start or couldn't open this folder: check
echo "wsl -l -v" for an Ubuntu 24.04 distro ^(SBX_WSL_DISTRO picks one^), and
echo run ./setup_linux.sh once from sbx_pc inside it.
if not defined SBX_NOPAUSE pause
exit /b 1

:failed
echo.
echo ===== FAILED. Last 40 lines of %LOG%:
powershell -NoProfile -Command "Get-Content -LiteralPath '%LOG%' -Tail 40"
echo.
echo ===== Lines mentioning errors:
findstr /i /n /c:"error" /c:"FAILED:" "%LOG%"
echo.
echo Full log: %LOG%
if not defined SBX_NOPAUSE pause
exit /b 1
