@echo off
rem ---------------------------------------------------------------------------
rem 手工链接 router.exe(绕开 vcxproj 的 LTCG/openssl_uptable.obj 冲突)。
rem 仅用于测试环境：让协议级测试能在本机跑起来，不改动产品工程。
rem
rem   用法: _link_router.bat
rem   退出码: 0=成功
rem ---------------------------------------------------------------------------

setlocal

set "TESTDIR=%~dp0"
set "ROUTERDIR=%~dp0.."
set "ROOT=%~dp0..\..\.."
set "OBJDIR=%ROOT%\_objs\router\Release"
set "LIBDIR=%ROOT%\libs"

set "VCVARS="
if exist "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS if exist "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat"

if not defined VCVARS (
	echo [ERROR] cannot locate vcvars64.bat
	exit /b 2
)

if not defined VSCMD_VER call "%VCVARS%" >nul

if not exist "%OBJDIR%\main.obj" (
	echo [ERROR] router objects not found: %OBJDIR%
	echo         run: MSBuild router.vcxproj /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v143 /p:BuildProjectReferences=false /p:WholeProgramOptimization=false /t:ClCompile
	exit /b 3
)

echo [LINK] router.exe
link /nologo /LTCG:OFF /machine:x64 /subsystem:console ^
	/out:"%TESTDIR%router.exe" ^
	/libpath:"%LIBDIR%" ^
	"%OBJDIR%\main.obj" "%OBJDIR%\router.obj" "%OBJDIR%\router_core.obj" "%OBJDIR%\actor_registry.obj" "%OBJDIR%\imp_stubs.obj" "%OBJDIR%\applink.obj" ^
	apr-1.lib aprutil-1.lib log4cxx.lib expat.lib Version.lib wldap32.lib netapi32.lib ^
	resmgr.lib server.lib xml.lib common.lib fmt.lib helper.lib math.lib network.lib ^
	libcurl.lib pyscript.lib python37.lib thread.lib ws2_32.lib

if errorlevel 1 exit /b 1

echo [LINK] done
exit /b 0
