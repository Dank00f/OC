@echo off
setlocal ENABLEDELAYEDEXPANSION
set REPO_DIR=%~1
if "%REPO_DIR%"=="" set REPO_DIR=%CD%
set BRANCH=%~2
if "%BRANCH%"=="" set BRANCH=main

echo [1/4] Updating repo...
git -C "%REPO_DIR%" fetch --all
git -C "%REPO_DIR%" checkout %BRANCH% 2>nul || git -C "%REPO_DIR%" checkout -b %BRANCH%
git -C "%REPO_DIR%" pull --ff-only

echo [2/4] Configuring (MinGW Makefiles)...
cmake -S "%REPO_DIR%" -B "%REPO_DIR%\build" -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Debug

echo [3/4] Building...
cmake --build "%REPO_DIR%\build" -j

echo [4/4] Running...
if exist "%REPO_DIR%\build\bin\hello.exe" (
  "%REPO_DIR%\build\bin\hello.exe"
) else if exist "%REPO_DIR%\build\hello.exe" (
  "%REPO_DIR%\build\hello.exe"
) else (
  echo Executable not found.
  exit /b 1
)
endlocal
