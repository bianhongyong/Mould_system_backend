add_library(
  config_core
  src/channel_topology_config.cpp
  src/launch_plan_config.cpp
)

target_include_directories(config_core PUBLIC include)
target_link_libraries(config_core PUBLIC common_build_flags)

find_package(GTest REQUIRED)
include(GoogleTest)

add_executable(
  launch_plan_config_test
  launch_plan_config_test.cpp
  test_launch_plan_gflags_registrar.cpp
)
target_link_libraries(launch_plan_config_test PRIVATE config_core common_build_flags)
add_test(NAME launch_plan_config_test COMMAND launch_plan_config_test)
set_tests_properties(launch_plan_config_test PROPERTIES LABELS "unit;launch_plan_config")

add_executable(
  launch_plan_fork_smoke
  launch_plan_fork_smoke.cpp
  test_launch_plan_gflags_registrar.cpp
)
target_link_libraries(launch_plan_fork_smoke PRIVATE config_core common_build_flags)
add_test(NAME launch_plan_fork_smoke COMMAND launch_plan_fork_smoke)
set_tests_properties(launch_plan_fork_smoke PROPERTIES LABELS "unit;launch_plan_config;multiprocess")

add_executable(launch_plan_registered_names_gtest launch_plan_registered_names_gtest.cpp)
target_link_libraries(
  launch_plan_registered_names_gtest
  PRIVATE
    config_core
    common_build_flags
    GTest::gtest_main
)
gtest_discover_tests(
  launch_plan_registered_names_gtest
  PROPERTIES LABELS "unit;launch_plan_config"
)

add_executable(channel_topology_config_gtest channel_topology_config_gtest.cpp)
target_link_libraries(
  channel_topology_config_gtest
  PRIVATE
    config_core
    common_build_flags
    GTest::gtest_main
)
gtest_discover_tests(
  channel_topology_config_gtest
  PROPERTIES LABELS "unit;channel_topology_config"
)
