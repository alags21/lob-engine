find_package(benchmark REQUIRED)

add_executable(lob_bench bench_smoke.cpp)
target_link_libraries(lob_bench PRIVATE lob lob_options benchmark::benchmark benchmark::benchmark_main)
