@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d C:\Users\Jerry\WorkSpace\Ruzino\build
ninja Ruzino
echo BUILDEXIT=%errorlevel%
