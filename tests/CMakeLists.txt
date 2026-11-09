find_package(GTest REQUIRED)
include(GoogleTest)

add_library(lob_reference STATIC reference/reference_book.cpp)
target_include_directories(lob_reference PUBLIC reference)
target_link_libraries(lob_reference PUBLIC lob PRIVATE lob_options)

add_library(lob_fuzzlib STATIC fuzz/fuzz.cpp)
target_include_directories(lob_fuzzlib PUBLIC fuzz)
target_link_libraries(lob_fuzzlib PUBLIC lob lob_reference PRIVATE lob_options)

add_executable(lob_tests test_smoke.cpp test_order_book.cpp test_event_log.cpp test_fuzz.cpp)
target_link_libraries(lob_tests PRIVATE lob lob_reference lob_fuzzlib lob_options GTest::gtest_main)
target_compile_definitions(lob_tests PRIVATE LOB_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
gtest_discover_tests(lob_tests)

add_executable(lob_fuzz fuzz/lob_fuzz_main.cpp)
target_link_libraries(lob_fuzz PRIVATE lob_fuzzlib lob_options)
