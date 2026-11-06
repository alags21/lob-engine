# lob_bench replaces global operator new to count allocations, which clashes with the ThreadSanitizer
# runtime's own replacement; it is single-threaded and has nothing for TSan to check.
if(NOT LOB_TSAN)
  add_executable(lob_bench bench_main.cpp)
  target_link_libraries(lob_bench PRIVATE lob lob_options)
endif()

add_executable(lob_mt_bench mt_bench.cpp)
target_link_libraries(lob_mt_bench PRIVATE lob lob_options)
