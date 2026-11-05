find_package(GTest REQUIRED)
include(GoogleTest)

add_executable(lob_tests test_smoke.cpp test_order_book.cpp)
target_link_libraries(lob_tests PRIVATE lob lob_options GTest::gtest_main)
gtest_discover_tests(lob_tests)
