add_executable(NewOutputTests
  TestNewOutputTypes.cpp
  TestEncryptedMemo.cpp
  TestDomainIndex.cpp
  TestNewOutputScanner.cpp
  TestBridgeMultisig.cpp
  TestIntegration.cpp
  ../../src/Sidechain/BridgeMultisigHandler.cpp
)

target_link_libraries(NewOutputTests
  CryptoNoteCore
  BoltCore
  BoltSync
  Serialization
  System
  Logging
  Common
  crypto
  gtest_main
  ${Boost_LIBRARIES}
)

if(${CMAKE_SYSTEM_NAME} STREQUAL "Linux" OR APPLE AND NOT ANDROID)
  target_link_libraries(NewOutputTests -lresolv)
endif()

set_property(TARGET NewOutputTests PROPERTY OUTPUT_NAME "new_output_tests")