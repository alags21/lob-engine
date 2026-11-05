add_executable(lob_cli lob_cli.cpp)
target_link_libraries(lob_cli PRIVATE lob lob_options)

add_executable(lob_replay lob_replay.cpp)
target_link_libraries(lob_replay PRIVATE lob lob_options)
