file(GLOB_RECURSE BOLTCORE_FILES BoltCore/*)

add_library(BoltCore STATIC ${BOLTCORE_FILES})

target_link_libraries(BoltCore PUBLIC CryptoNoteCore Common crypto ${Boost_LIBRARIES})