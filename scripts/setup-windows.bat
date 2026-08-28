@echo off
setlocal enabledelayedexpansion
title ESP-Switch Environment Setup (Windows)
rem ============================================================
rem  ESP-Switch build environment setup for Windows
rem  Targets: arduino-cli-web console (C3 / ESP8285 / C2 builds)
rem  Idempotent: safe to re-run, skips what is already installed.
rem  NOTE: keep this file ASCII-only to avoid codepage issues.
rem ============================================================

set "ADB_DIR=%LOCALAPPDATA%\Programs\arduino-cli"
set "ARDUINO15=%USERPROFILE%\.arduino15"
set "CLI_YAML=%ARDUINO15%\arduino-cli.yaml"

echo ==========================================
echo   ESP-Switch Environment Setup (Windows)
echo ==========================================
echo.

rem ---------- 1. Node.js ----------
echo [1/5] Checking Node.js ...
where node >nul 2>nul
if %errorlevel%==0 (
  for /f "delims=" %%v in ('node --version') do set "NODE_VER=%%v"
  for /f "tokens=1 delims=." %%a in ("!NODE_VER!") do set "NODE_MAJOR=%%a"
  set "NODE_MAJOR=!NODE_MAJOR:v=!"
  echo   [OK] Node.js found: !NODE_VER!
  if !NODE_MAJOR! LSS 14 (
    echo   [WARN] Node.js is too old ^(^<14^), arduino-cli-web requires v14+. Please upgrade.
  )
) else (
  echo   [..] Node.js not found.
  where winget >nul 2>nul
  if !errorlevel!==0 (
    echo   Installing Node.js LTS via winget ...
    winget install --id OpenJS.NodeJS.LTS -e --accept-source-agreements --accept-package-agreements
    if !errorlevel!==0 (
      echo   [OK] Node.js installed. New terminal may be required for PATH.
    ) else (
      echo   [FAIL] winget install failed. Install Node.js LTS manually from https://nodejs.org
    )
  ) else (
    echo   [FAIL] winget not available. Install Node.js LTS manually from https://nodejs.org
  )
)

rem ---------- 2. arduino-cli ----------
echo.
echo [2/5] Checking arduino-cli ...
where arduino-cli >nul 2>nul
if %errorlevel%==0 (
  for /f "delims=" %%v in ('arduino-cli version') do set "CLI_VER=%%v"
  echo   [OK] arduino-cli found: !CLI_VER!
) else if exist "%ADB_DIR%\arduino-cli.exe" (
  echo   [OK] arduino-cli found at: %ADB_DIR%
) else (
  echo   [..] arduino-cli not found.
  where winget >nul 2>nul
  if !errorlevel!==0 (
    echo   Installing arduino-cli via winget ...
    winget install --id Arduino.arduino-cli -e --accept-source-agreements --accept-package-agreements
    if !errorlevel!==0 (
      echo   [OK] arduino-cli installed via winget.
    ) else (
      echo   [..] winget failed, falling back to direct download ...
      call :install_arduino_cli_direct
    )
  ) else (
    call :install_arduino_cli_direct
  )
)

rem ---------- 3. arduino-cli config yaml ----------
echo.
echo [3/5] Checking arduino-cli config ...
if exist "%CLI_YAML%" (
  echo   [OK] config exists: %CLI_YAML%
) else (
  echo   [..] creating %CLI_YAML% ...
  if not exist "%ARDUINO15%" mkdir "%ARDUINO15%"
  > "%CLI_YAML%" (
    echo directories:
    echo   data: %ARDUINO15%
    echo   downloads: %ARDUINO15%\staging
    echo board_manager:
    echo   additional_urls:
    echo     - https://espressif.github.io/arduino-esp32/package_esp32_index.json
    echo     - https://arduino.esp8266.com/stable/package_esp8266com_index.json
  )
  echo   [OK] config created.
)

rem ---------- 4. optional: install cores (big download) ----------
echo.
echo [4/5] Arduino cores (esp32 / esp8266) - optional, large download.
set /p INSTALL_CORES="  Install cores now? [y/N]: "
if /i "!INSTALL_CORES!"=="y" (
  where arduino-cli >nul 2>nul
  if !errorlevel!==0 (
    set "CLI_CMD=arduino-cli"
  ) else if exist "%ADB_DIR%\arduino-cli.exe" (
    set "CLI_CMD=%ADB_DIR%\arduino-cli.exe"
  ) else (
    set "CLI_CMD="
  )
  if defined CLI_CMD (
    rem skip cores that are already installed (avoid re-download)
    "!CLI_CMD!" --config-file "%CLI_YAML%" core list > "%TEMP%\esp_core_list.txt" 2>nul
    set "NEED_CORES="
    findstr /i /c:"esp32:esp32 " "%TEMP%\esp_core_list.txt" >nul || set "NEED_CORES=!NEED_CORES! esp32:esp32"
    findstr /i /c:"esp8266:esp8266" "%TEMP%\esp_core_list.txt" >nul || set "NEED_CORES=!NEED_CORES! esp8266:esp8266"
    if defined NEED_CORES (
      echo   Updating core index ...
      "!CLI_CMD!" --config-file "%CLI_YAML%" core update-index
      echo   Installing missing cores:!NEED_CORES!
      "!CLI_CMD!" --config-file "%CLI_YAML%" core install!NEED_CORES!
      echo   [OK] cores installed.
    ) else (
      echo   [OK] esp32:esp32 and esp8266:esp8266 already installed, skip.
    )
  ) else (
    echo   [SKIP] arduino-cli not available yet, skip core install.
  )
) else (
  echo   [SKIP] core install skipped. Re-run with 'y' to install.
)

rem ---------- 5. ESP-IDF (optional, for C2 builds) ----------
echo.
echo [5/5] ESP-IDF (needed for ESP32-C2 builds) - optional, ~3GB.
if defined IDF_PATH (
  echo   [OK] IDF_PATH=%IDF_PATH%
) else if exist "D:\Espressif\frameworks" (
  echo   [OK] ESP-IDF found at D:\Espressif (official installer layout).
) else if exist "C:\Espressif\frameworks" (
  echo   [OK] ESP-IDF found at C:\Espressif (official installer layout).
) else (
  echo   [..] ESP-IDF not detected.
  echo        For C2 builds install the official ESP-IDF Windows installer:
  echo        https://dl.espressif.com/dl/esp-idf/
  echo        (choose ESP-IDF v5.5.x, it supports esp32c2 target)
)

echo.
echo ==========================================
echo   Environment summary
echo ==========================================
node --version 2>nul && echo   node: OK
where arduino-cli >nul 2>nul && echo   arduino-cli: OK (PATH)
if exist "%ADB_DIR%\arduino-cli.exe" echo   arduino-cli: OK (%ADB_DIR%)
if defined IDF_PATH echo   IDF_PATH=%IDF_PATH%
echo.
echo   Start the web console:
echo     cd arduino-cli-web ^&^& node server.js
echo     or double-click arduino-cli-web\start.bat
echo   Then open http://localhost:8787
echo.
pause
exit /b 0

rem ============================================================
rem  Subroutine: direct download of arduino-cli (latest release)
rem ============================================================
:install_arduino_cli_direct
echo   Downloading latest arduino-cli ...
if not exist "%ADB_DIR%" mkdir "%ADB_DIR%"
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$r = Invoke-RestMethod -Uri 'https://api.github.com/repos/arduino/arduino-cli/releases/latest'; " ^
  "$a = $r.assets | Where-Object { $_.name -match 'Windows_64bit.*\\.zip$' } | Select-Object -First 1; " ^
  "if (-not $a) { Write-Host 'No matching asset found'; exit 1 }; " ^
  "Invoke-WebRequest -Uri $a.browser_download_url -OutFile \"%ADB_DIR%\\arduino-cli.zip\"; " ^
  "Expand-Archive -Path \"%ADB_DIR%\\arduino-cli.zip\" -DestinationPath \"%ADB_DIR%\" -Force"
if %errorlevel%==0 (
  rem move the exe from extracted subfolder to the root
  for /f "delims=" %%f in ('dir /b /s "%ADB_DIR%\arduino-cli.exe" 2^>nul') do (
    copy /y "%%f" "%ADB_DIR%\arduino-cli.exe" >nul
  )
  rem add to user PATH if not present
  set "OLD_PATH="
  for /f "skip=2 tokens=2,*" %%a in ('reg query "HKCU\Environment" /v Path 2^>nul') do set "OLD_PATH=%%b"
  echo !OLD_PATH! | find /i "%ADB_DIR%" >nul
  if !errorlevel!==0 (
    if defined OLD_PATH (
      setx PATH "!OLD_PATH!;%ADB_DIR%" >nul
    ) else (
      setx PATH "%ADB_DIR%" >nul
    )
    echo   [OK] arduino-cli installed and added to user PATH (new terminal required).
  ) else (
    echo   [OK] arduino-cli installed (already in PATH).
  )
  del /q "%ADB_DIR%\arduino-cli.zip" 2>nul
) else (
  echo   [FAIL] download failed. Get arduino-cli manually from:
  echo          https://github.com/arduino/arduino-cli/releases
)
exit /b 0
