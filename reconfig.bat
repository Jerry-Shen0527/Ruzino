@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d C:\Users\Jerry\WorkSpace\Ruzino\build
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DRUZINO_WITH_CUDA=ON -DUSTC_HOMEWORK_PLUGINS=OFF .. 2>&1
echo CONFIGEXIT=%errorlevel%
