@echo off
:: Initialize and verify this repository's committed public dependencies.
setlocal EnableExtensions EnableDelayedExpansion

set "SCRIPT_DIR=%~dp0"
cd /d "%SCRIPT_DIR%"
rem N64Recomp contains an optional Ares gitlink whose historical object is not
rem available from the public Ares repository. Skip only that oracle subtree.
git -c submodule.ares-bridge/third_party/ares.update=none submodule update --init --recursive
if errorlevel 1 exit /b 1

set "DECOMP_DIR=%SCRIPT_DIR%disasm"
set "N64RECOMP_DIR=%SCRIPT_DIR%n64recomp"
set "RUNTIME_DIR=%SCRIPT_DIR%lib\N64ModernRuntime"
set "SDL2_DIR=%SCRIPT_DIR%lib\SDL2"
set "ADRENOTOOLS_DIR=%SCRIPT_DIR%lib\adrenotools"
set "RT64_DIR=%SCRIPT_DIR%lib\rt64"
set "UI_DIR=%SCRIPT_DIR%recomp-ui"
set "ARES_DIR=%N64RECOMP_DIR%\ares-bridge\third_party\ares"

for %%D in ("%DECOMP_DIR%" "%N64RECOMP_DIR%" "%RUNTIME_DIR%" "%SDL2_DIR%" "%ADRENOTOOLS_DIR%" "%RT64_DIR%" "%UI_DIR%") do (
    if not exist "%%~D\.git" (
        echo Error: required repository is missing: %%~D
        echo Run git submodule update --init --recursive from %SCRIPT_DIR%.
        exit /b 1
    )
    git -C "%%~D" rev-parse HEAD >nul 2>&1
    if errorlevel 1 (
        echo Error: required repository has no checked-out commit: %%~D
        exit /b 1
    )
)

set "SHA="
for /f "usebackq tokens=1,* delims==" %%A in ("%SCRIPT_DIR%n64recomp.pin") do (
    set "KEY=%%A"
    set "KEY=!KEY: =!"
    if /I "!KEY!"=="sha" (
        set "SHA=%%B"
        set "SHA=!SHA: =!"
    )
)
if not defined SHA (
    echo Error: no sha found in n64recomp.pin
    exit /b 1
)

for /f %%H in ('git -C "%N64RECOMP_DIR%" rev-parse HEAD') do set "ACTUAL=%%H"
if not defined ACTUAL (
    echo Error: could not read N64Recomp HEAD from %N64RECOMP_DIR%
    exit /b 1
)
if /I not "!ACTUAL!"=="!SHA!" (
    echo Note: n64recomp HEAD ^(!ACTUAL!^) differs from the committed pin ^(!SHA!^).
)

set "ROM_PATH=%DECOMP_DIR%\baseroms\us\baserom.z64"
if not exist "%ROM_PATH%" echo Note: place your legal Stadium 2 US ROM at %ROM_PATH%

if /I "%WITH_ARES%"=="1" (
    if not exist "%ARES_DIR%\.git" (
        echo Note: optional Ares gitlink is unavailable; continuing without it.
    ) else (
        git -C "%ARES_DIR%" rev-parse HEAD >nul 2>&1
        if errorlevel 1 (
            echo Note: optional Ares gitlink is unavailable; continuing without it.
        ) else (
            echo Ares checkout is available.
        )
    )
)

echo Applying dependency patches (tools\apply_dependency_patches.py)...
py -3 tools\apply_dependency_patches.py
if errorlevel 1 (
    echo Error: dependency patches failed to apply. See the messages above.
    exit /b 1
)

echo.
echo Repository dependencies are available.
for /f %%H in ('git -C "%DECOMP_DIR%" rev-parse --short HEAD') do echo   pokestadiumgs: %%H
for /f %%H in ('git -C "%N64RECOMP_DIR%" rev-parse --short HEAD') do echo   N64Recomp:     %%H
echo.
echo Build the disassembly from: %DECOMP_DIR%
echo Configure the game from this repository:
echo   cmake -S . -B build

endlocal
