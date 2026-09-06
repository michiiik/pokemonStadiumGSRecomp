@echo off
:: Verify the committed public submodules in the shared workspace.
setlocal EnableExtensions EnableDelayedExpansion

for %%I in ("%~dp0..\..") do set "WORKSPACE_ROOT=%%~fI"
set "DECOMP_DIR=%WORKSPACE_ROOT%\decomp\pokestadiumgs"
set "N64RECOMP_DIR=%WORKSPACE_ROOT%\toolchain\N64Recomp"
set "RUNTIME_DIR=%WORKSPACE_ROOT%\toolchain\N64ModernRuntime"
set "RT64_DIR=%WORKSPACE_ROOT%\toolchain\rt64"
set "UI_DIR=%WORKSPACE_ROOT%\toolchain\recomp-ui"
set "ARES_DIR=%WORKSPACE_ROOT%\toolchain\ares"

for %%D in ("%DECOMP_DIR%" "%N64RECOMP_DIR%" "%RUNTIME_DIR%" "%RT64_DIR%" "%UI_DIR%") do (
    git -C "%%~D" rev-parse --git-dir >nul 2>&1
    if errorlevel 1 (
        echo Error: required workspace repository is missing: %%~D
        echo Initialize the committed public submodules from %WORKSPACE_ROOT%.
        exit /b 1
    )
)

set "SHA="
for /f "usebackq tokens=1,* delims==" %%A in ("%~dp0n64recomp.pin") do (
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
    echo Note: N64Recomp HEAD ^(!ACTUAL!^) differs from the game pin ^(!SHA!^).
)

set "ROM_PATH=%DECOMP_DIR%\baseroms\us\baserom.z64"
if not exist "%ROM_PATH%" echo Note: place your legal Stadium 2 US ROM at %ROM_PATH%

if /I "%WITH_ARES%"=="1" (
    git -C "%ARES_DIR%" rev-parse --git-dir >nul 2>&1
    if errorlevel 1 (
        echo Note: optional Ares submodule is not initialized; continuing without it.
    ) else (
        echo Ares submodule is available.
    )
)

echo.
echo Workspace dependencies are available.
for /f %%H in ('git -C "%DECOMP_DIR%" rev-parse --short HEAD') do echo   pokestadiumgs: %%H
for /f %%H in ('git -C "%N64RECOMP_DIR%" rev-parse --short HEAD') do echo   N64Recomp:     %%H
echo.
echo Build the disassembly from: %DECOMP_DIR%
echo Configure the game from the workspace root:
echo   cmake -S games/pokemonStadiumGSRecomp -B build/games/stadium2

endlocal
