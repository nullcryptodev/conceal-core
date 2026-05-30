file(GLOB_RECURSE BOLT_TUI_FILES BoltClientWallet/*)

add_executable(BoltClientWallet ${BOLT_TUI_FILES})

add_dependencies(BoltClientWallet version)
set_property(TARGET BoltClientWallet PROPERTY OUTPUT_NAME "conceal-bolt")