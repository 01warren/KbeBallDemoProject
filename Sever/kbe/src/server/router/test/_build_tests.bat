@echo off
rem ---------------------------------------------------------------------------
rem 编译 router 单元测试(Windows)。
rem 可从任意命令行运行：脚本会自行初始化 VS 构建环境。
rem 产物(.exe/.obj/.pdb)生成在本脚本所在目录，可用 _clean_tests.bat 清理。
rem
rem   用法: _build_tests.bat
rem   退出码: 0=成功 1=编译失败 2=找不到 VS 构建环境
rem ---------------------------------------------------------------------------

setlocal

set "TESTDIR=%~dp0"
set "KBE_SRC=%~dp0..\..\.."

set "VCVARS="
if exist "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS if exist "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat"

if not defined VCVARS (
	echo [ERROR] cannot locate vcvars64.bat, please run from a VS developer prompt.
	exit /b 2
)

if not defined VSCMD_VER call "%VCVARS%" >nul

cd /d "%TESTDIR%"

rem router_mail.h 经 common/common.h 间接依赖 fmt，这里补上其 include 路径
set "INC=/I"%KBE_SRC%" /I"%KBE_SRC%\lib" /I"%KBE_SRC%\lib\dependencies" /I"%KBE_SRC%\lib\dependencies\fmt\include""

echo [BUILD] router_registry_test
cl /nologo /EHsc /W3 %INC% router_registry_test.cpp ..\actor_registry.cpp /Fe:router_registry_test.exe
if errorlevel 1 exit /b 1

echo [BUILD] router_protocol_test
cl /nologo /EHsc /W3 %INC% router_protocol_test.cpp ..\actor_registry.cpp /Fe:router_protocol_test.exe
if errorlevel 1 exit /b 1

echo [BUILD] router_mail_test
cl /nologo /EHsc /W3 %INC% router_mail_test.cpp /Fe:router_mail_test.exe
if errorlevel 1 exit /b 1

rem L3 协议客户端：零 KBE 依赖，只需要系统 socket 库
echo [BUILD] router_proto_client
cl /nologo /EHsc /W3 %INC% router_proto_client.cpp /Fe:router_proto_client.exe /link ws2_32.lib
if errorlevel 1 exit /b 1

echo [BUILD] done
exit /b 0
