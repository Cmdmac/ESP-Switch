@echo off
setlocal enabledelayedexpansion
title ESP-Switch Environment Setup (Windows)
rem ============================================================
rem  ESP-Switch build environment setup for Windows
rem  Targets: arduino-cli-web console (C3 / ESP8285 / C2 builds)
rem  Idempotent: safe to re-run, skips what is already installed.
rem  NOTE: keep this file ASCII-only to avoid codepage issues.
rem  Step order kept in sync with scripts/setup-linux.sh:
rem    [1/5] Node.js  [2/5] ESP-IDF  [3/5] arduino-cli  [4/5] config  [5/5] cores
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
  set "INSTALL_NODE="
  set /p "INSTALL_NODE=  Node.js is required. Download and install Node.js LTS now? [y/N]: "
  if /i not "!INSTALL_NODE!"=="y" (
    echo   [SKIP] Node.js not installed. Install it manually from https://nodejs.org
    goto :after_node
  )
  echo   [..] Installing Node.js LTS ...
  where winget >nul 2>nul
  if !errorlevel!==0 (
    echo   Installing via winget ...
    winget install --id OpenJS.NodeJS.LTS -e --accept-source-agreements --accept-package-agreements
    if !errorlevel!==0 (
      echo   [OK] Node.js installed via winget. New terminal may be required for PATH.
      goto :after_node
    )
    echo   [..] winget install failed, falling back to direct download ...
  )
  call :install_node_direct
)
:after_node

rem ---------- 2. ESP-IDF (optional, for C2 builds) ----------
echo.
echo [2/5] ESP-IDF ^(needed for ESP32-C2 builds^) - optional, ~3GB.
if defined IDF_PATH (
  echo   [OK] IDF_PATH=%IDF_PATH%
) else if exist "D:\Espressif\frameworks" (
  echo   [OK] ESP-IDF found at D:\Espressif ^(official installer layout^).
) else if exist "C:\Espressif\frameworks" (
  echo   [OK] ESP-IDF found at C:\Espressif ^(official installer layout^).
) else (
  echo   [..] ESP-IDF not detected.
  echo        For C2 builds install the official ESP-IDF Windows installer:
  echo        https://dl.espressif.com/dl/esp-idf/
  echo        ^(choose ESP-IDF v5.5.x, it supports esp32c2 target^)
)

rem ---------- 3. arduino-cli ----------
echo.
echo [3/5] Checking arduino-cli ...
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
    winget install --id ArduinoSA.CLI -e --accept-source-agreements --accept-package-agreements
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

rem ---------- 4. arduino-cli config yaml ----------
rem Prefer the Arduino IDE 2.x data dir (%LOCALAPPDATA%\Arduino15) so the
rem CLI reuses cores already installed by the IDE; fall back to the
rem arduino-cli / IDE 1.x default (%USERPROFILE%\.arduino15).
set "CLI_DATA=%ARDUINO15%"
if exist "%LOCALAPPDATA%\Arduino15\packages\esp32\hardware\esp32" set "CLI_DATA=%LOCALAPPDATA%\Arduino15"
if exist "%LOCALAPPDATA%\Arduino15\packages\esp8266\hardware\esp8266" set "CLI_DATA=%LOCALAPPDATA%\Arduino15"
echo.
echo [4/5] Checking arduino-cli config ...
echo   data dir: !CLI_DATA!
if exist "%CLI_YAML%" (
  > "%TEMP%\esp_yaml.txt" type "%CLI_YAML%"
  findstr /i /c:"data: !CLI_DATA!" "%TEMP%\esp_yaml.txt" >nul
  if errorlevel 1 (
    echo   [..] updating config to point at !CLI_DATA! ...
    > "%CLI_YAML%" (
      echo directories:
      echo   data: !CLI_DATA!
      echo   downloads: !CLI_DATA!\staging
      echo board_manager:
      echo   additional_urls:
      echo     - https://espressif.github.io/arduino-esp32/package_esp32_index.json
      echo     - https://arduino.esp8266.com/stable/package_esp8266com_index.json
    )
    echo   [OK] config updated.
  ) else (
    echo   [OK] config exists and points at the right data dir.
  )
  del /q "%TEMP%\esp_yaml.txt" >nul 2>nul
) else (
  echo   [..] creating %CLI_YAML% ...
  if not exist "!CLI_DATA!" mkdir "!CLI_DATA!"
  > "%CLI_YAML%" (
    echo directories:
    echo   data: !CLI_DATA!
    echo   downloads: !CLI_DATA!\staging
    echo board_manager:
    echo   additional_urls:
    echo     - https://espressif.github.io/arduino-esp32/package_esp32_index.json
    echo     - https://arduino.esp8266.com/stable/package_esp8266com_index.json
  )
  echo   [OK] config created.
)

rem ---------- 5. optional: install cores (big download) ----------
rem If step 3 installed arduino-cli via winget this run, its path is not yet in the
rem current session PATH. To avoid clobbering an arduino-cli already usable in the
rem session PATH (which may be absent from the registry, so refresh_path would drop
rem it), the registry reload is only a fallback done below when arduino-cli is missing.
echo.
echo [5/5] Arduino cores (esp32 / esp8266) - optional, large download.
rem Detect cores already installed by Arduino IDE 2.x (%LOCALAPPDATA%\Arduino15)
rem or by arduino-cli / Arduino IDE 1.x (%USERPROFILE%\.arduino15) so we do not
rem re-download them.
set "HAVE_ESP32="
set "HAVE_ESP8266="
for %%D in ("%LOCALAPPDATA%\Arduino15" "%USERPROFILE%\.arduino15") do (
  if exist "%%~D\packages\esp32\hardware\esp32" set "HAVE_ESP32=1"
  if exist "%%~D\packages\esp8266\hardware\esp8266" set "HAVE_ESP8266=1"
)
if defined HAVE_ESP32 (
  echo   [OK] esp32 core already installed ^(Arduino IDE / arduino-cli^).
) else (
  echo   [..] esp32 core not found.
)
if defined HAVE_ESP8266 (
  echo   [OK] esp8266 core already installed ^(Arduino IDE / arduino-cli^).
) else (
  echo   [..] esp8266 core not found.
)
if defined HAVE_ESP32 if defined HAVE_ESP8266 (
  echo   [OK] All cores present, nothing to install.
  goto :after_cores
)
set /p INSTALL_CORES="  Install missing cores now? [y/N]: "
if /i "!INSTALL_CORES!"=="y" (
  set "CLI_CMD="
  rem Prefer an arduino-cli already usable in the session PATH (confirmed in step 3).
  rem Only if missing there, reload PATH from the registry (covers a winget install this
  rem run), then retry; finally fall back to the known dir %ADB_DIR%.
  where arduino-cli >nul 2>nul
  if !errorlevel!==0 (
    set "CLI_CMD=arduino-cli"
  ) else (
    call :refresh_path
    where arduino-cli >nul 2>nul
    if !errorlevel!==0 (
      set "CLI_CMD=arduino-cli"
    ) else if exist "%ADB_DIR%\arduino-cli.exe" (
      set "CLI_CMD=%ADB_DIR%\arduino-cli.exe"
    )
  )
  if defined CLI_CMD (
    echo   Updating core index ...
    "!CLI_CMD!" --config-file "%CLI_YAML%" core update-index
    if not defined HAVE_ESP32 (
      echo   Installing esp32:esp32 ^(large download^) ...
      "!CLI_CMD!" --config-file "%CLI_YAML%" core install esp32:esp32
    )
    if not defined HAVE_ESP8266 (
      echo   Installing esp8266:esp8266 ...
      "!CLI_CMD!" --config-file "%CLI_YAML%" core install esp8266:esp8266
    )
    echo   [OK] cores installed.
  ) else (
    echo   [SKIP] arduino-cli not available.
    echo          Either install arduino-cli, or install the missing cores from
    echo          Arduino IDE: Tools -^> Board -^> Boards Manager -^> search "esp32".
  )
) else (
  echo   [SKIP] core install skipped. Re-run with 'y' to install.
)
:after_cores

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
rem  Subroutine: direct download of Node.js LTS (no winget required)
rem  Resolves the newest LTS version from nodejs.org index.json,
rem  downloads the win-x64 zip, extracts it under
rem  %LOCALAPPDATA%\Programs\node-<ver>-win-x64 and adds that folder
rem  to the user PATH (also prepends it to this session's PATH so the
rem  summary below can verify node immediately).
rem ============================================================
:install_node_direct
if not exist "%LOCALAPPDATA%\Programs" mkdir "%LOCALAPPDATA%\Programs" >nul 2>nul
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12; " ^
  "try { " ^
  "  $j = Invoke-RestMethod -Uri 'https://nodejs.org/dist/index.json' -UseBasicParsing; " ^
  "  $ver = ($j | Where-Object { $_.lts } | Select-Object -First 1).version; " ^
  "  if (-not $ver) { Write-Host '  [FAIL] could not resolve latest LTS version'; exit 1 } " ^
  "  $nodeDir = Join-Path $env:LOCALAPPDATA ('Programs\node-' + $ver + '-win-x64'); " ^
  "  Write-Host ('  latest LTS: ' + $ver); " ^
  "  if (-not (Test-Path (Join-Path $nodeDir 'node.exe'))) { " ^
  "    $zip = Join-Path $env:TEMP ('node-' + $ver + '-win-x64.zip'); " ^
  "    Invoke-WebRequest -Uri ('https://nodejs.org/dist/' + $ver + '/node-' + $ver + '-win-x64.zip') -OutFile $zip -UseBasicParsing; " ^
  "    Expand-Archive -Path $zip -DestinationPath (Join-Path $env:LOCALAPPDATA 'Programs') -Force; " ^
  "    Remove-Item $zip -Force -ErrorAction SilentlyContinue; " ^
  "  } " ^
  "  if (-not (Test-Path (Join-Path $nodeDir 'node.exe'))) { Write-Host '  [FAIL] node.exe missing after extract'; exit 1 } " ^
  "  Write-Host ('  node.exe: ' + (Join-Path $nodeDir 'node.exe')); " ^
  "  $userPath = [Environment]::GetEnvironmentVariable('Path','User'); " ^
  "  if (-not $userPath) { $userPath = '' } " ^
  "  if (-not (($userPath -split ';') -contains $nodeDir)) { " ^
  "    [Environment]::SetEnvironmentVariable('Path', ($userPath.TrimEnd(';') + ';' + $nodeDir), 'User'); " ^
  "    Write-Host '  added to user PATH (new terminal required)'; " ^
  "  } else { Write-Host '  already in user PATH'; } " ^
  "} catch { " ^
  "  Write-Host ('  [FAIL] ' + $_.Exception.Message); " ^
  "  exit 1 " ^
  "}"
if not "!errorlevel!"=="0" goto node_direct_failed
exit /b 0

:node_direct_failed
echo   [FAIL] Node.js download/install failed. Install it manually from:
echo          https://nodejs.org
exit /b 1

rem ============================================================
rem  Subroutine: direct download of arduino-cli
rem  Uses the official stable URL on downloads.arduino.cc:
rem  no GitHub API (no rate limit, no JSON parsing), works with TLS 1.2.
rem ============================================================
:install_arduino_cli_direct
echo   Downloading arduino-cli (latest) ...
if not exist "%ADB_DIR%" mkdir "%ADB_DIR%"
set "CLI_ZIP=%TEMP%\arduino-cli-win64.zip"
if exist "%CLI_ZIP%" del /q "%CLI_ZIP%" >nul 2>nul

powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12; " ^
  "try { " ^
  "  Invoke-WebRequest -Uri 'https://downloads.arduino.cc/arduino-cli/arduino-cli_latest_Windows_64bit.zip' -OutFile '%CLI_ZIP%' -UseBasicParsing; " ^
  "  Write-Host '  downloaded'; " ^
  "} catch { " ^
  "  Write-Host ('  [FAIL] ' + $_.Exception.Message); " ^
  "  exit 1 " ^
  "}"
rem NOTE: use delayed expansion here - inside a parenthesized if/else block
rem cmd expands %errorlevel% while parsing, before the command above runs.
if not "!errorlevel!"=="0" goto cli_direct_failed
if not exist "%CLI_ZIP%" goto cli_direct_failed

echo   Extracting to %ADB_DIR% ...
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "try { " ^
  "  Expand-Archive -Path '%CLI_ZIP%' -DestinationPath '%ADB_DIR%' -Force; " ^
  "  Write-Host '  extracted'; " ^
  "} catch { " ^
  "  Write-Host ('  [FAIL] ' + $_.Exception.Message); " ^
  "  exit 1 " ^
  "}"
if not "!errorlevel!"=="0" goto cli_direct_failed

del /q "%CLI_ZIP%" >nul 2>nul
call :add_adb_dir_to_path
exit /b 0

:cli_direct_failed
echo   [FAIL] download/extract failed. Install arduino-cli manually from:
echo          https://github.com/arduino/arduino-cli/releases
echo          (or: winget install --id ArduinoSA.CLI -e)
exit /b 1

rem ============================================================
rem  Subroutine: reload PATH from the registry into this session
rem ============================================================
:refresh_path
set "SYS_PATH="
set "USER_PATH="
for /f "skip=2 tokens=2,*" %%a in ('reg query "HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Environment" /v Path 2^>nul') do (
  if not defined SYS_PATH set "SYS_PATH=%%b"
)
for /f "skip=2 tokens=2,*" %%a in ('reg query "HKCU\Environment" /v Path 2^>nul') do (
  if not defined USER_PATH set "USER_PATH=%%b"
)
if defined SYS_PATH if defined USER_PATH set "PATH=!SYS_PATH!;!USER_PATH!"
if defined SYS_PATH if not defined USER_PATH set "PATH=!SYS_PATH!"
exit /b 0

rem ============================================================
rem  Subroutine: add %ADB_DIR% to the user PATH (once)
rem  The PATH value commonly contains parentheses, e.g.
rem  "C:\Program Files (x86)\...". Piping such a value through
rem  `echo !VAR! | find ...` re-parses it and breaks cmd
rem  ("The syntax of the command is incorrect"), so the value is
rem  written to a temp file and checked with findstr instead.
rem ============================================================
:add_adb_dir_to_path
set "CURPATH="
for /f "skip=2 tokens=2,*" %%a in ('reg query "HKCU\Environment" /v Path 2^>nul') do (
  if not defined CURPATH set "CURPATH=%%b"
)
set "PATHCHK=%TEMP%\esp_switch_path.txt"
> "%PATHCHK%" echo(!CURPATH!
findstr /i /c:"%ADB_DIR%" "%PATHCHK%" >nul
if errorlevel 1 (
  if defined CURPATH (
    setx PATH "!CURPATH!;%ADB_DIR%" >nul
  ) else (
    setx PATH "%ADB_DIR%" >nul
  )
  echo   [OK] arduino-cli installed and added to user PATH (new terminal required).
) else (
  echo   [OK] arduino-cli installed (already in PATH).
)
del /q "%PATHCHK%" >nul 2>nul
exit /b 0
