cmake_minimum_required(VERSION 3.20)
project(OpenAnsel VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_TOOLCHAIN_FILE)
    if(DEFINED ENV{VCPKG_ROOT})
        set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
    endif()
endif()

find_package(minhook CONFIG REQUIRED)
find_package(directxtex CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)

# Backend selection — set OFF at configure time to exclude a backend.
# The x86 build (DX9 / Mirror's Edge etc.) should set -DOPENANSEL_BUILD_DXGI=OFF.
# The x64 build (DX11/12 / modern games) should set -DOPENANSEL_BUILD_DX9=OFF.
option(OPENANSEL_BUILD_DX9  "Build the DX9 proxy backend (d3d9.dll)"  ON)
option(OPENANSEL_BUILD_DXGI "Build the DXGI proxy backend (dxgi.dll)" ON)

add_subdirectory(src)
