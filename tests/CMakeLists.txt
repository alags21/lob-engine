find_package(GTest REQUIRED)
include(GoogleTest)

add_executable(lob_tests test_smoke.cpp)
target_link_libraries(lob_tests PRIVATE lob lob_options GTest::gtest_main)
gtest_discover_tests(lob_tests)
