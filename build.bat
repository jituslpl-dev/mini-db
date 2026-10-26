@echo off
setlocal enabledelayedexpansion

echo [mini-db] Building project...

:: Check if g++ is on PATH
where g++ >nul 2>nul
if %ERRORLEVEL% equ 0 (
    set GPP=g++
    goto :COMPILE
)

:: Check CLion bundled MinGW
if exist "D:\CLion 2026.2.2\bin\mingw\bin\g++.exe" (
    set "GPP=D:\CLion 2026.2.2\bin\mingw\bin\g++.exe"
    goto :COMPILE
)

:: Fallback check WinLibs
for /d %%D in ("C:\Users\%USERNAME%\AppData\Local\Microsoft\WinGet\Packages\BrechtSanders.WinLibs*") do (
    if exist "%%D\mingw64\bin\g++.exe" (
        set "GPP=%%D\mingw64\bin\g++.exe"
        goto :COMPILE
    )
)

echo ERROR: Could not find g++ compiler. Please ensure MinGW or GCC is installed and on PATH.
exit /b 1

:COMPILE
echo Using compiler: %GPP%
"%GPP%" -std=c++17 -O2 -Isrc -pthread ^
 src/common/types.cpp ^
 src/common/schema.cpp ^
 src/storage/disk_manager.cpp ^
 src/storage/lru_k_replacer.cpp ^
 src/storage/buffer_pool.cpp ^
 src/storage/heap_table.cpp ^
 src/index/b_plus_tree.cpp ^
 src/concurrency/lock_manager.cpp ^
 src/concurrency/transaction.cpp ^
 src/recovery/log_manager.cpp ^
 src/recovery/recovery_manager.cpp ^
 src/catalog/catalog.cpp ^
 src/parser/sql_parser.cpp ^
 src/execution/executor.cpp ^
 src/execution/planner.cpp ^
 src/cli/cli.cpp ^
 -o minidb_cli.exe

if %ERRORLEVEL% equ 0 (
    echo [mini-db] Build successful: minidb_cli.exe generated.
) else (
    echo [mini-db] Build failed with error code %ERRORLEVEL%.
)
exit /b %ERRORLEVEL%
