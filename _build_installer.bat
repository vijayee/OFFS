@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" || exit /b 1

set BUILD=%~dp0cmake-build-msvc
set STAGE=%~dp0packaging\windows\_stage

cd /d "%BUILD%" || exit /b 1
ninja offsd offs_cli offs-updater offs-ca || (exit /b 1)

echo === staging ===
copy /y offsd.exe "%STAGE%\offs-daemon.exe" || exit /b 1
copy /y offs.exe "%STAGE%\offs.exe" || exit /b 1
copy /y offs-updater.exe "%STAGE%\offs-updater.exe" || exit /b 1
copy /y deps\liboffs\tools\offs-ca\offs-ca.exe "%STAGE%\offs-ca.exe" || exit /b 1
copy /y msquic.dll "%STAGE%\msquic.dll" || exit /b 1

echo === wix build ===
cd /d "%~dp0" || exit /b 1
rem Every build mints a monotonic ProductVersion (0.1.<commit count>), so
rem MajorUpgrade in offs.wxs replaces the previous install cleanly instead of
rem piling up same-version registrations (the shared-Component GUIDs kept old
rem binaries on "REINSTALL" - ProductCode changes per wix build).
for /f %%C in ('git rev-list --count HEAD') do set PRODUCT_VERSION=0.1.%%C
if "%PRODUCT_VERSION%"=="" (echo === could not derive PRODUCT_VERSION && exit /b 1)
echo === ProductVersion %PRODUCT_VERSION% ===
wix build -arch x64 -d ProductVersion=%PRODUCT_VERSION% -ext WixToolset.UI.wixext -ext WixToolset.Firewall.wixext packaging\windows\_stage\offs.wxs -b packaging\windows\_stage -o "%STAGE%\offs-%PRODUCT_VERSION%.msi" || exit /b 1
echo === DONE: %STAGE%\offs-%PRODUCT_VERSION%.msi ===