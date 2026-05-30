# Conceal tools
# ColdSync - standalone wallet pre-syncer
# MigrationTool - blockchain migration utility
# Optimizer - fusion transaction optimizer
# HealthCheck - daemon health monitoring utility

# Optimizer
add_executable(Optimizer Optimizer.cpp)
target_link_libraries(Optimizer BoltHttp Rpc HTTP Blockchain CryptoNoteCore Storage Logging Serialization crypto System Common
    ${Boost_LIBRARIES}
)
add_dependencies(Optimizer version)
set_property(TARGET Optimizer PROPERTY OUTPUT_NAME "conceal-optimizer")

# ColdSync — uses StateManager + SyncManager from BoltRpcLib
add_executable(ColdSync ColdSync.cpp)
target_link_libraries(ColdSync
    BoltRpcLib
    BoltCore
    BoltSync
    BoltHttp
    NodeRpcProxy
    Rpc
    HTTP
    CryptoNoteCore
    Storage
    System
    Logging
    crypto
    Serialization
    Mnemonics
    Common
    ${Boost_LIBRARIES}
)
add_dependencies(ColdSync version)
set_property(TARGET ColdSync PROPERTY OUTPUT_NAME "conceal-coldsync")

if (${CMAKE_SYSTEM_NAME} STREQUAL "Linux" OR APPLE AND NOT ANDROID)
  target_link_libraries(ColdSync -lresolv)
endif()

# MigrationTool
add_executable(MigrationTool MigrationTool.cpp)
target_link_libraries(MigrationTool CryptoNoteCore Storage P2P Rpc System HTTP Logging Common crypto Serialization
  ${Boost_LIBRARIES}
)
add_dependencies(MigrationTool version)
set_property(TARGET MigrationTool PROPERTY OUTPUT_NAME "conceald-migrate")

# HealthCheck — standalone daemon health monitor
add_executable(HealthCheck HealthCheck.cpp)
set_property(TARGET HealthCheck PROPERTY OUTPUT_NAME "conceal-health")

# Migration GUI (optional, requires wxWidgets)
add_subdirectory(Migration-gui)