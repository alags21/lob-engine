add_executable(lob_bench bench_main.cpp)
target_link_libraries(lob_bench PRIVATE lob lob_options)

add_executable(lob_mt_bench mt_bench.cpp)
target_link_libraries(lob_mt_bench PRIVATE lob lob_options)
