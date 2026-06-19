# conceald-migrate-gui — wxWidgets front-end for conceald-migrate
#
# Build: cmake -DBUILD_MIGRATION_GUI=ON ..
# Optional: -DMIGRATION_GUI_AUTO_INSTALL_DEPS=OFF to skip apt/dnf/brew attempts

if(NOT BUILD_MIGRATION_GUI)
  return()
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/FindOrInstallWxWidgets.cmake")
conceal_find_or_install_wxwidgets()

set(MIGRATION_GUI_SOURCES
  MigrationGuiMain.cpp
  MigrationGuiApp.cpp
  MigrationDialog.cpp
  MigratePaths.cpp
)

add_executable(MigrationToolGui ${MIGRATION_GUI_SOURCES})
target_include_directories(MigrationToolGui PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

if(_wxwidgetsUseCmakeTargets)
  # MSYS2 ships wxWidgetsConfig.cmake with imported targets (wx::core, wx::base).
  target_link_libraries(MigrationToolGui PRIVATE wx::core wx::base)
else()
  target_include_directories(MigrationToolGui PRIVATE ${wxWidgets_INCLUDE_DIRS})
  target_compile_definitions(MigrationToolGui PRIVATE ${wxWidgets_DEFINITIONS})
  if(wxWidgets_CXX_FLAGS)
    target_compile_options(MigrationToolGui PRIVATE ${wxWidgets_CXX_FLAGS})
  endif()

  # wxMSW links version.lib; avoid CMake resolving "version" to our version UTILITY target.
  set(_migrationGuiWxLibs)
  foreach(_wxLib ${wxWidgets_LIBRARIES})
    if(MSVC AND _wxLib STREQUAL "version")
      list(APPEND _migrationGuiWxLibs "version.lib")
    else()
      list(APPEND _migrationGuiWxLibs ${_wxLib})
    endif()
  endforeach()
  target_link_libraries(MigrationToolGui PRIVATE ${_migrationGuiWxLibs})

  if(MSVC)
    target_link_libraries(MigrationToolGui PRIVATE shlwapi.lib uxtheme.lib)
  endif()
endif()

add_dependencies(MigrationToolGui version)
set_property(TARGET MigrationToolGui PROPERTY OUTPUT_NAME "conceald-migrate-gui")

if(WIN32)
  set_target_properties(MigrationToolGui PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

message(STATUS "Migration GUI enabled: conceald-migrate-gui")
