add_subdirectory(pow)

add_definitions(-DMINIUPNP_STATICLIB)
include_directories(SYSTEM ${CMAKE_SOURCE_DIR}/external/parallel_hashmap)
include_directories(${MDBX_INCLUDE_DIR})

file(GLOB_RECURSE MDBX_STORAGE_FILES Storage/*)
file(GLOB_RECURSE Blockchain_FILES Blockchain/*)
file(GLOB_RECURSE BlockchainExplorer BlockchainExplorer/*)
file(GLOB_RECURSE Common Common/*)
file(GLOB_RECURSE crypto crypto/*)
file(GLOB_RECURSE CryptoNoteCore CryptoNoteCore/* CryptoNoteConfig.h)
file(GLOB_RECURSE CryptoNoteProtocol CryptoNoteProtocol/*)
file(GLOB_RECURSE Daemon Daemon/*)
file(GLOB_RECURSE HTTP HTTP/*)
file(GLOB_RECURSE InProcessNode InProcessNode/*)
file(GLOB_RECURSE Logging Logging/*)
file(GLOB_RECURSE Mnemonics Mnemonics/*)
file(GLOB_RECURSE NodeRpcProxy NodeRpcProxy/*)
file(GLOB_RECURSE P2p P2p/*)
file(GLOB_RECURSE Rpc Rpc/*)
file(GLOB_RECURSE Serialization Serialization/*)
file(GLOB_RECURSE ConcealWallet ConcealWallet/*)
file(GLOB_RECURSE Transfers Transfers/*)
file(GLOB_RECURSE Wallet Wallet/*)
file(GLOB_RECURSE WalletLegacy WalletLegacy/*)
file(GLOB_RECURSE JsonRpcServer JsonRpcServer/*)

file(GLOB_RECURSE BOLTSYNC_FILES BoltSync/*)
file(GLOB_RECURSE BOLTCORE_FILES BoltCore/*)
file(GLOB_RECURSE CLIENT_WALLET_FILES ClientWallet/*)
file(GLOB_RECURSE BOLTRPC_FILES BoltRPC/*)
file(GLOB_RECURSE SIDECHAIN_FILES Sidechain/*)
file(GLOB_RECURSE BOLTHTTP_FILES BoltHttp/*)
file(GLOB_RECURSE NODECLIENT_FILES NodeClient/*)

if(MSVC OR MINGW)
  file(GLOB_RECURSE System System/* Platform/Windows/System/*)
elseif(APPLE)
  file(GLOB_RECURSE System System/* Platform/OSX/System/*)
else()
  file(GLOB_RECURSE System System/* Platform/Linux/System/*)
endif()

source_group("" FILES ${Common} ${ConcealWallet} ${crypto} ${CryptoNoteCore} ${CryptoNoteProtocol} ${Daemon} ${JsonRpcServer} ${HTTP} ${Logging} ${NodeRpcProxy} ${P2p} ${Rpc} ${Serialization} ${System} ${Transfers} ${Wallet} ${WalletLegacy})
source_group("Storage" FILES ${MDBX_STORAGE_FILES})
source_group("Blockchain" FILES ${Blockchain_FILES})
source_group("BoltRPC" FILES ${BOLTRPC_FILES})
source_group("BoltSync" FILES ${BOLTSYNC_FILES})
source_group("BoltCore" FILES ${BOLTCORE_FILES})
source_group("BoltHttp" FILES ${BOLTHTTP_FILES})
source_group("NodeClient" FILES ${NODECLIENT_FILES})
source_group("Sidechain" FILES ${SIDECHAIN_FILES})
source_group("ClientWallet" FILES ${CLIENT_WALLET_FILES})

# ── Libraries ──────────────────────────────────────────────────────────────
add_library(Blockchain STATIC ${Blockchain_FILES})
add_library(BlockchainExplorer STATIC ${BlockchainExplorer})
add_library(Common STATIC ${Common})
add_library(crypto STATIC ${crypto})
add_library(CryptoNoteCore STATIC ${CryptoNoteCore})
target_link_libraries(CryptoNoteCore PUBLIC pow Common crypto Serialization Logging)
add_library(HTTP STATIC ${HTTP})
add_library(InProcessNode STATIC ${InProcessNode})
add_library(Logging STATIC ${Logging})
add_library(Mnemonics STATIC ${Mnemonics})
add_library(NodeRpcProxy STATIC ${NodeRpcProxy})
add_library(Rpc STATIC ${Rpc})
add_library(P2P STATIC ${CryptoNoteProtocol} ${P2p})
target_link_libraries(P2P PUBLIC pow CryptoNoteCore)
add_library(Serialization STATIC ${Serialization})
add_library(System STATIC ${System})
add_library(Transfers STATIC ${Transfers})
add_library(Wallet STATIC ${Wallet} ${WalletLegacy})
add_library(JsonRpcServer STATIC ${JsonRpcServer})
add_library(BoltSync STATIC ${BOLTSYNC_FILES})
add_library(BoltCore STATIC ${BOLTCORE_FILES})
add_library(BoltHttp STATIC ${BOLTHTTP_FILES})

# NodeClient — clean INode implementation using BoltHttp (no legacy coroutines)
add_library(NodeClientLib STATIC ${NODECLIENT_FILES})
target_link_libraries(NodeClientLib PUBLIC
    BoltHttp
    CryptoNoteCore
    Serialization
    Common
    crypto
    ${Boost_LIBRARIES}
)
add_dependencies(NodeClientLib version)

add_library(Storage STATIC ${MDBX_STORAGE_FILES})
add_dependencies(Storage version)
target_link_libraries(Storage PRIVATE ${MDBX_LIBRARY_TARGET} Blockchain Serialization Common crypto)

# Sidechain static library
add_library(SidechainLib STATIC ${SIDECHAIN_FILES})
target_link_libraries(SidechainLib PUBLIC Blockchain NodeRpcProxy System Rpc HTTP BoltSync BoltCore BoltHttp CryptoNoteCore Serialization
  -Wl,--start-group
    Common
    Logging
    crypto
  -Wl,--end-group
  ${Boost_LIBRARIES} ${CMAKE_THREAD_LIBS_INIT})
add_dependencies(SidechainLib version)

# BoltRPC static library — uses NodeClientLib instead of NodeRpcProxy
add_library(BoltRpcLib STATIC ${BOLTRPC_FILES})
target_link_libraries(BoltRpcLib PUBLIC
    Blockchain
    BoltSync
    BoltCore
    BoltHttp
    NodeClientLib
    Mnemonics
    Rpc
    HTTP
    System
    CryptoNoteCore
    Storage
    Serialization
    ${Boost_LIBRARIES}
    ${CMAKE_THREAD_LIBS_INIT}
    -Wl,--start-group
    Common
    Logging
    crypto
    -Wl,--end-group
)
add_dependencies(BoltRpcLib version)

# ── Link dependencies ─────────────────────────────────────────────────────
target_link_libraries(Blockchain PUBLIC CryptoNoteCore Storage Common crypto Serialization Logging pow ${Boost_LIBRARIES})
add_dependencies(Blockchain version)

target_link_libraries(BoltSync PUBLIC BoltCore Blockchain CryptoNoteCore Storage Common crypto Serialization ${CMAKE_THREAD_LIBS_INIT})
target_link_libraries(BoltCore PUBLIC Blockchain CryptoNoteCore Common crypto ${Boost_LIBRARIES})

# ── Executables ───────────────────────────────────────────────────────────

add_executable(Daemon ${Daemon})
set_property(TARGET Daemon PROPERTY OUTPUT_NAME "conceald")
add_dependencies(Daemon version)

add_executable(ConcealWallet ${ConcealWallet})
add_executable(BoltRPC ${BOLTRPC_FILES})
add_executable(Sidechain ${SIDECHAIN_FILES})

# ── ClientWallet (new TUI wallet) ─────────────────────────────────────────
add_executable(ClientWallet ${CLIENT_WALLET_FILES})
set_property(TARGET ClientWallet PROPERTY OUTPUT_NAME "conceal-wallet")
add_dependencies(ClientWallet version)

if (MSVC)
  target_link_libraries(System ws2_32)
endif ()

if (MINGW)
  target_link_libraries(Common dnsapi)
  target_link_libraries(P2P bcrypt)
  target_link_libraries(System ws2_32)
endif ()

# ── Link executables ──────────────────────────────────────────────────────

macro(link_daemon TARGET_NAME)
  target_link_libraries(${TARGET_NAME}
    CryptoNoteCore Blockchain Storage P2P Rpc System HTTP Logging Common crypto pow
    libminiupnpc-static BlockchainExplorer ${Boost_LIBRARIES} Serialization)
  if (${CMAKE_SYSTEM_NAME} STREQUAL "Linux" OR APPLE AND NOT ANDROID)
    target_link_libraries(${TARGET_NAME} -lresolv)
  endif()
endmacro()

link_daemon(Daemon)

target_link_libraries(ConcealWallet Wallet NodeRpcProxy Transfers Rpc HTTP CryptoNoteCore Blockchain Storage System Logging Common crypto ${Boost_LIBRARIES} Serialization Mnemonics)

# ── ClientWallet — uses NodeClientLib instead of NodeRpcProxy ─────────────
target_link_libraries(ClientWallet
    BoltCore
    BoltSync
    BoltRpcLib
    NodeClientLib
    Storage
    Blockchain
    CryptoNoteCore
    Rpc
    HTTP
    System
    Serialization
    ${CMAKE_THREAD_LIBS_INIT}
    -Wl,--start-group
    Common
    Logging
    crypto
    ${Boost_LIBRARIES}
    -Wl,--end-group
)

# ── BoltRPC — uses NodeClientLib instead of NodeRpcProxy ──────────────────
target_link_libraries(BoltRPC
    BoltRpcLib
    BoltSync
    BoltCore
    BoltHttp
    NodeClientLib
    Mnemonics
    Rpc
    HTTP
    System
    Blockchain
    Storage
    CryptoNoteCore
    Serialization
    ${CMAKE_THREAD_LIBS_INIT}
    -Wl,--start-group
    Common
    Logging
    crypto
    ${Boost_LIBRARIES}
    -Wl,--end-group
)

# ── Sidechain ─────────────────────────────────────────────────────────────
target_link_libraries(Sidechain BoltSync BoltCore BoltHttp NodeRpcProxy Rpc HTTP System Blockchain CryptoNoteCore Serialization
  -Wl,--start-group
      Common
      Logging
      crypto
  -Wl,--end-group
  ${Boost_LIBRARIES}
  ${CMAKE_THREAD_LIBS_INIT}
)

# Tools
add_subdirectory(Tools)

if (${CMAKE_SYSTEM_NAME} STREQUAL "Linux" OR APPLE AND NOT ANDROID)
  target_link_libraries(ConcealWallet -lresolv)
  target_link_libraries(ClientWallet -lresolv)
  target_link_libraries(BoltRPC -lresolv)
  target_link_libraries(Sidechain -lresolv)
endif ()

# ── Dependencies ──────────────────────────────────────────────────────────
add_dependencies(Rpc version)
add_dependencies(ConcealWallet version)
add_dependencies(P2P version)
add_dependencies(BoltRPC version)
add_dependencies(Sidechain version)
add_dependencies(Storage version)
add_dependencies(BoltRpcLib version)
add_dependencies(NodeClientLib version)
add_dependencies(ClientWallet version)

set_property(TARGET ConcealWallet PROPERTY OUTPUT_NAME "concealwallet")
set_property(TARGET BoltRPC PROPERTY OUTPUT_NAME "conceal-rpc")
set_property(TARGET Sidechain PROPERTY OUTPUT_NAME "conceal-side")