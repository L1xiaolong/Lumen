# libjxl publishes pkg-config metadata but no CMake package on all supported platforms.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(PC_JpegXL QUIET libjxl)
endif()

find_path(JpegXL_INCLUDE_DIR NAMES jxl/decode.h HINTS ${PC_JpegXL_INCLUDE_DIRS})
find_library(JpegXL_LIBRARY_RELEASE NAMES jxl HINTS ${PC_JpegXL_LIBRARY_DIRS})
if(VCPKG_INSTALLED_DIR AND VCPKG_TARGET_TRIPLET)
    find_library(JpegXL_LIBRARY_DEBUG NAMES jxl jxld
        PATHS "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/lib" NO_DEFAULT_PATH)
endif()

include(SelectLibraryConfigurations)
select_library_configurations(JpegXL)
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(JpegXL REQUIRED_VARS JpegXL_LIBRARY JpegXL_INCLUDE_DIR
    VERSION_VAR PC_JpegXL_VERSION)

if(JpegXL_FOUND AND NOT TARGET JpegXL::JpegXL)
    add_library(JpegXL::JpegXL INTERFACE IMPORTED)
    target_include_directories(JpegXL::JpegXL INTERFACE "${JpegXL_INCLUDE_DIR}")
    target_link_libraries(JpegXL::JpegXL INTERFACE ${JpegXL_LIBRARIES})
endif()
mark_as_advanced(JpegXL_INCLUDE_DIR)
