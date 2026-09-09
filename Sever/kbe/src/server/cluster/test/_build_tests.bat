@echo off
rem ============================================================
rem _build_tests.bat -- 构建 cluster 测试工具(test_client/snapshot_gen/dummy_proc)
rem
rem 依赖: Visual Studio C++ 工具链(cl)。脚本自动探测常见 VS 安装路径,
rem       优先使用 x64 开发者环境(与 smoke_client 构建一致)。
rem 用法: 直接运行本脚本(无需手工开 VS 命令行)。
rem 输出: test_client.exe / snapshot_gen.exe / dummy_proc.exe 于本目录。
rem ============================================================
setlocal EnableDelayedExpansion

set "TEST_DIR=%~dp0"
for %%I in ("%TEST_DIR%..") do set "CLUSTER_DIR=%%~fI"

rem ---- 定位 vcvarsall.bat ----
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VCVARS="
if exist "%VSWHERE%" (
    for /f "usebackq delims=" %%v in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VCROOT=%%v"
    if defined VCROOT if exist "%VCROOT%\VC\Auxiliary\Build\vcvarsall.bat" set "VCVARS=%VCROOT%\VC\Auxiliary\Build\vcvarsall.bat"
)

if not defined VCVARS (
    for %%p in (
        "C:\Program Files\Microsoft Visual Studio\2022\BuildTools"
        "C:\Program Files\Microsoft Visual Studio\2022\Community"
        "C:\Program Files\Microsoft Visual Studio\2022\Professional"
        "C:\Program Files\Microsoft Visual Studio\2022\Enterprise"
        "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
        "C:\Program Files (x86)\Microsoft Visual Studio\2022\Community"
        "C:\Program Files (x86)\Microsoft Visual Studio\2022\Professional"
        "C:\Program Files (x86)\Microsoft Visual Studio\2022\Enterprise"
        "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools"
        "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community"
        "C:\Program Files (x86)\Microsoft Visual Studio\2019\Professional"
        "C:\Program Files (x86)\Microsoft Visual Studio\2019\Enterprise"
    ) do (
        if not defined VCVARS if exist "%%~p\VC\Auxiliary\Build\vcvarsall.bat" set "VCVARS=%%~p\VC\Auxiliary\Build\vcvarsall.bat"
    )
)

if not defined VCVARS (
    echo [error] cannot locate vcvarsall.bat ^(Visual Studio C++ Build Tools^)
    exit /b 3
)

call "%VCVARS%" x64 >nul 2>&1
if errorlevel 1 (
    call "%VCVARS%" x86 >nul 2>&1
    if errorlevel 1 (
        echo [error] vcvarsall.bat failed: "%VCVARS%"
        exit /b 3
    )
)

echo [info] vcvars=%VCVARS%
echo [info] compiling test_client.cpp ...
cl /nologo /EHsc /W3 /I"%CLUSTER_DIR%" "%TEST_DIR%test_client.cpp" /Fe:"%TEST_DIR%test_client.exe" /link ws2_32.lib
if errorlevel 1 ( echo [error] test_client build failed & exit /b 1 )

echo [info] compiling snapshot_gen.cpp ...
cl /nologo /EHsc /W3 /I"%CLUSTER_DIR%" "%TEST_DIR%snapshot_gen.cpp" /Fe:"%TEST_DIR%snapshot_gen.exe" /link ws2_32.lib
if errorlevel 1 ( echo [error] snapshot_gen build failed & exit /b 1 )

echo [info] compiling dummy_proc.cpp ...
cl /nologo /EHsc /W3 /I"%CLUSTER_DIR%" "%TEST_DIR%dummy_proc.cpp" /Fe:"%TEST_DIR%dummy_proc.exe" /link user32.lib
if errorlevel 1 ( echo [error] dummy_proc build failed & exit /b 1 )

echo [ok] all test tools built under %TEST_DIR%
endlocal
