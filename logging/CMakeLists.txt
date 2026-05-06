add_library(
  logging_core
  src/log_collector.cpp
  src/log_dump_manager.cpp
  src/log_pipe_manager.cpp
  src/log_sink.cpp
  src/logging_fork_gflags.cpp
  src/subprocess_log_init.cpp
)

target_include_directories(
  logging_core
  PUBLIC
    include
)
target_link_libraries(
  logging_core
  PUBLIC
    comm_core
    config_core
    common_build_flags
)

# zlib for log compression (optional)
find_package(ZLIB QUIET)
if(ZLIB_FOUND)
  target_link_libraries(logging_core PRIVATE ZLIB::ZLIB)
  target_compile_definitions(logging_core PRIVATE MOULD_HAVE_ZLIB=1)
endif()

find_package(GTest REQUIRED)
include(GoogleTest)

add_executable(log_pipeline_test log_pipeline_test.cpp)
target_include_directories(log_pipeline_test PRIVATE include)
target_link_libraries(
  log_pipeline_test
  PRIVATE
    logging_core
    common_build_flags
    GTest::gtest_main
)
gtest_discover_tests(
  log_pipeline_test
  PROPERTIES LABELS "unit;log_pipeline"
)
