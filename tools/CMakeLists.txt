add_executable(lob_replay lob_replay.cpp)
target_link_libraries(lob_replay PRIVATE lob lob_options)

add_executable(lob_sim lob_sim.cpp)
target_link_libraries(lob_sim PRIVATE lob lob_options)
