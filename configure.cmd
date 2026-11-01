@echo off
setlocal
set "CMAKE=C:\Program Files\CMake\bin\cmake.exe"
set "NINJA=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
where "%CMAKE%" >nul 2>&1 || set "CMAKE=cmake"
if not exist "%NINJA%" set "NINJA=ninja"
"%CMAKE%" -S "%~dp0." -B "%~dp0build" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_MAKE_PROGRAM="%NINJA%"
exit /b %ERRORLEVEL%
