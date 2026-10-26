@echo off
setlocal

if not exist minidb_cli.exe (
    echo minidb_cli.exe not found. Running build.bat first...
    call build.bat
    if %ERRORLEVEL% neq 0 (
        echo Build failed. Exiting.
        exit /b 1
    )
)

del demo.db demo.db.log 2>nul
minidb_cli.exe demo.db < demo_run.sql > demo_output.txt 2>&1
echo EXIT CODE: %ERRORLEVEL% >> demo_output.txt
type demo_output.txt
