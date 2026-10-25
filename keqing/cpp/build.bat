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
rem 编到临时名再替换：边车是**常驻进程**，宿主随时占着 driver.exe，
rem 直接 /Fe:driver.exe 会在链接期报 LNK1104（无法打开文件）——症状是"构建失败"，
rem 而真正原因（文件被运行中的进程占用）藏在链接器输出里。实测踩过。
cl /nologo /utf-8 /O2 /EHsc /std:c++17 /I"%LANGDIR%stb" main.cpp /Fe:driver_tmp.exe /Fo:driver_tmp.obj
if errorlevel 1 (
  popd
  set "FAIL=1"
  echo [build.bat] %PROJ% compile FAILED 1>&2
  goto :eof
)
del /q driver_tmp.obj >nul 2>&1
del /q driver.obj >nul 2>&1
rem 替换：先试删旧；删不掉（被运行中的宿主持有）就挪成 .old，再放新的。
rem 这样"构建"总能产出可用的 driver.exe，不必等宿主释放。
del /q driver.exe >nul 2>&1
if exist driver.exe move /y driver.exe driver_old.exe >nul 2>&1
move /y driver_tmp.exe driver.exe >nul 2>&1
if not exist driver.exe (
  popd
  set "FAIL=1"
  echo [build.bat] %PROJ% install FAILED -^> driver.exe still locked 1>&2
  goto :eof
)
del /q driver_old.exe >nul 2>&1
popd
echo [build.bat] %PROJ% built -^> driver.exe
goto :eof
