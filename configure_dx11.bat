@echo off
REM configure_dx11.bat - Configure x64 build for DX11/DX12 (openansel_dxgi.dll -> dxgi.dll)
REM This is a SEPARATE build tree from the x86 DX9 build.

echo [OpenAnsel] Configuring x64 (DX11/DX12) build...
set VCPKG_ROOT=C:\vcpkg

cmake -B "C:\Users\Anakin\OpenAnsel\build64" ^
      -S "C:\Users\Anakin\OpenAnsel" ^
      -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake ^
      -DVCPKG_TARGET_TRIPLET=x64-windows ^
      -A x64 ^
      -DOPENANSEL_BUILD_DX9=OFF ^
      -DOPENANSEL_BUILD_DXGI=ON

echo [OpenAnsel] Done. Build with: cmake --build build64 --config Release --target openansel_dxgi
pause
