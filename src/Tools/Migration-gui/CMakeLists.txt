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
target_include_directories(MigrationToolGui PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${wxWidgets_INCLUDE_DIRS}
)
target_compile_definitions(MigrationToolGui PRIVATE ${wxWidgets_DEFINITIONS})
if(wxWidgets_CXX_FLAGS)
  target_compile_options(MigrationToolGui PRIVATE ${wxWidgets_CXX_FLAGS})
endif()
target_link_libraries(MigrationToolGui PRIVATE ${wxWidgets_LIBRARIES})
add_dependencies(MigrationToolGui version)
set_property(TARGET MigrationToolGui PROPERTY OUTPUT_NAME "conceald-migrate-gui")

if(WIN32)
  set_target_properties(MigrationToolGui PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

message(STATUS "Migration GUI enabled: conceald-migrate-gui")
