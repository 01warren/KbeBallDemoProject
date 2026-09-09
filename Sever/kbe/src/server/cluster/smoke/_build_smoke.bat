@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d "F:\KBEngine\BallProject\KbeBallDemoProject\Sever\kbe\src\server\cluster"
cl /nologo /EHsc /I"F:\KBEngine\BallProject\KbeBallDemoProject\Sever\kbe\src\server\cluster" smoke_client.cpp /Fe:"F:\KBEngine\BallProject\KbeBallDemoProject\Sever\kbe\src\server\cluster\smoke\smoke_client.exe" /link ws2_32.lib
