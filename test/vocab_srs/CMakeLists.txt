add_executable(VocabSrsTest
  VocabSrsTest.cpp
)

target_include_directories(VocabSrsTest PRIVATE
  ${REPO_ROOT}/lib/Vocab
)

target_link_libraries(VocabSrsTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(VocabSrsTest)
