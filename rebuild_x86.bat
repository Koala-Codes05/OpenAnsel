@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
set VCPKG_ROOT=C:\vcpkg
rmdir /s /q C:\Users\Anakin\OpenAnsel\build
cmake -B C:\Users\Anakin\OpenAnsel\build -S C:\Users\Anakin\OpenAnsel -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake -A Win32 -DCMAKE_GENERATOR_PLATFORM=Win32
cmake --build C:\Users\Anakin\OpenAnsel\build --config Release
