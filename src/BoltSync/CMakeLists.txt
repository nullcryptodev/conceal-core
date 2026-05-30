file(GLOB_RECURSE BOLTSYNC_FILES BoltSync/*)

add_library(BoltSync STATIC ${BOLTSYNC_FILES})

target_link_libraries(BoltSync PUBLIC CryptoNoteCore Storage Common crypto Serialization ${CMAKE_THREAD_LIBS_INIT})