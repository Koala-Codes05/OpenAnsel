#----------------------------------------------------------------
# Generated CMake target import file for configuration "Debug".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "Microsoft::DirectXTex" for configuration "Debug"
set_property(TARGET Microsoft::DirectXTex APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG)
set_target_properties(Microsoft::DirectXTex PROPERTIES
  IMPORTED_IMPLIB_DEBUG "${_IMPORT_PREFIX}/debug/lib/DirectXTex.lib"
  IMPORTED_LOCATION_DEBUG "${_IMPORT_PREFIX}/debug/bin/DirectXTex.dll"
  )

list(APPEND _cmake_import_check_targets Microsoft::DirectXTex )
list(APPEND _cmake_import_check_files_for_Microsoft::DirectXTex "${_IMPORT_PREFIX}/debug/lib/DirectXTex.lib" "${_IMPORT_PREFIX}/debug/bin/DirectXTex.dll" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
