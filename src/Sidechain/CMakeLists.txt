target_link_libraries(Sidechain
    BoltSync
    BoltCore
    NodeRpcProxy
    Rpc
    HTTP
    System
    CryptoNoteCore
    Serialization
    ${CN_LINK_GROUP_START}
        Common
        Logging
        crypto
    ${CN_LINK_GROUP_END}
    ${Boost_LIBRARIES}
    ${CMAKE_THREAD_LIBS_INIT}
)

if (${CMAKE_SYSTEM_NAME} STREQUAL "Linux" OR APPLE AND NOT ANDROID)
    target_link_libraries(Sidechain -lresolv)
endif()

add_dependencies(Sidechain version)
set_property(TARGET Sidechain PROPERTY OUTPUT_NAME "conceal-side")