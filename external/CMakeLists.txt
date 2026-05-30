option(ENABLE_MDBX "Enable MDBX database backend" ON)

set(UPNPC_BUILD_STATIC ON CACHE BOOL "Build static library")
set(UPNPC_BUILD_SHARED OFF CACHE BOOL "Build shared library")
set(UPNPC_BUILD_TESTS OFF CACHE BOOL "Build test executables")

add_subdirectory(miniupnpc)

if (DO_TESTS)
  add_subdirectory(gtest)
endif()

if(ENABLE_MDBX)
    set(MDBX_BUILD_TOOLS OFF CACHE BOOL "Disable MDBX tools")
    set(MDBX_BUILD_TESTS OFF CACHE BOOL "Disable MDBX tests")

    set(CMAKE_C_FLAGS_BAK "${CMAKE_C_FLAGS}")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-undef -Wno-logical-op -Wno-aggregate-return -Wno-nested-externs -Wno-discarded-qualifiers -Wno-strict-prototypes -Wno-old-style-definition")

    add_subdirectory(mdbx)
    
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS_BAK}")

    if(TARGET mdbx-static)
      set_target_properties(mdbx-static PROPERTIES SYSTEM TRUE)
    endif()
    set(MDBX_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/mdbx" PARENT_SCOPE)
    set(MDBX_LIBRARY_TARGET mdbx-static PARENT_SCOPE)
endif()

set_property(TARGET libminiupnpc-static PROPERTY FOLDER "external")

if(MSVC)
  set_property(TARGET libminiupnpc-static APPEND_STRING PROPERTY COMPILE_FLAGS " -wd4244 -wd4267")
elseif(NOT MSVC)
  set_property(TARGET libminiupnpc-static APPEND_STRING PROPERTY COMPILE_FLAGS " -Wno-undef -Wno-unused-result -Wno-unused-value")
endif()

if(DO_TESTS)
  set_property(TARGET gtest gtest_main PROPERTY FOLDER "external")
endif()

if(ENABLE_MDBX AND TARGET mdbx)
  set(MDBX_LIBRARY_TARGET mdbx PARENT_SCOPE)
endif()
