@echo off
rem C++ language-dir build script: compile each agent project's main.cpp to driver.exe with MSVC cl.
rem Usage: build.bat [project-dir-name]  -- default: iterate all agent projects
rem Requires VS Build Tools; vswhere locates vcvars64.bat. Host build-bootstrap calls this script.
setlocal

set "LANGDIR=%~dp0"
call :locate_vcvars
if not "%VCVARS%"=="" goto :have_compiler
echo [build.bat] MSVC not found - install VS Build Tools with C++ workload 1>&2
exit /b 1

:have_compiler
call "%VCVARS%" >nul 2>&1
if not "%ERRORLEVEL%"=="0" (
  echo [build.bat] vcvars64 init failed 1>&2
  exit /b 1
)

set "FAIL=0"
if not "%~1"=="" (
  call :build_one "%~1"
) else (
  for /d %%D in ("%LANGDIR%*") do (
    if exist "%%D\main.cpp" call :build_one "%%~nxD"
  )
)
exit /b %FAIL%

:locate_vcvars
set "VCVARS="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :eof
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do (
  if exist "%%i\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
)
goto :eof

:build_one
set "PROJ=%~1"
if not exist "%LANGDIR%%PROJ%\main.cpp" (
  echo [build.bat] skip %PROJ% - no main.cpp 1>&2
  goto :eof
)
echo [build.bat] compiling %PROJ% ...
pushd "%LANGDIR%%PROJ%"
cl /nologo /utf-8 /O2 /EHsc /std:c++17 /I"%LANGDIR%stb" main.cpp /Fe:driver.exe /Fo:driver.obj
if errorlevel 1 (
  popd
  set "FAIL=1"
  echo [build.bat] %PROJ% compile FAILED 1>&2
  goto :eof
)
popd
del /q "%LANGDIR%%PROJ%\driver.obj" >nul 2>&1
echo [build.bat] %PROJ% built -^> driver.exe
goto :eof
