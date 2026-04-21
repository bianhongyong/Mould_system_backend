add_library(
  comm_core
  src/callback_queue.cpp
  src/channel_factory.cpp
  src/image_pipeline.cpp
  src/module_factory_registry.cpp
  src/module_base.cpp
  src/payload_codec.cpp
  src/ready_pipe_protocol.cpp
  src/restart_policy.cpp
  src/refcounted_message_store.cpp
  src/reliability.cpp
  src/supervisor.cpp
  src/shm_bus_control_plane.cpp
  src/shm_bus_runtime_prefork.cpp
  src/shm_ring_buffer.cpp
  src/shm_segment.cpp
  src/shm_bus_runtime.cpp
)

target_include_directories(comm_core PUBLIC include)
target_link_libraries(comm_core PUBLIC common_build_flags config_core)
if(TARGET absl::status)
  target_link_libraries(comm_core PUBLIC absl::status)
endif()
if(TARGET absl::statusor)
  target_link_libraries(comm_core PUBLIC absl::statusor)
endif()

find_package(GTest REQUIRED)
include(GoogleTest)

add_executable(shm_segment_test shm_segment_test.cpp)
target_link_libraries(shm_segment_test PRIVATE comm_core common_build_flags)
add_test(NAME shm_segment_test COMMAND shm_segment_test)
set_tests_properties(shm_segment_test PROPERTIES LABELS "unit;shm_segment")

add_executable(shm_ring_buffer_layout_test shm_ring_buffer_layout_test.cpp)
target_link_libraries(
  shm_ring_buffer_layout_test
  PRIVATE
    comm_core
    common_build_flags
    GTest::gtest
)
gtest_discover_tests(
  shm_ring_buffer_layout_test
  PROPERTIES LABELS "unit;shm_ring_buffer"
)

add_executable(multi_consumer_cursor_reclaim_test multi_consumer_cursor_reclaim_test.cpp)
target_link_libraries(multi_consumer_cursor_reclaim_test PRIVATE comm_core common_build_flags)
add_test(NAME multi_consumer_cursor_reclaim_test COMMAND multi_consumer_cursor_reclaim_test)
set_tests_properties(multi_consumer_cursor_reclaim_test PROPERTIES LABELS "unit;shm_pubsub;ring")

add_executable(daemon_membership_observability_test daemon_membership_observability_test.cpp)
target_link_libraries(daemon_membership_observability_test PRIVATE comm_core common_build_flags)
add_test(NAME daemon_membership_observability_test COMMAND daemon_membership_observability_test)
set_tests_properties(daemon_membership_observability_test PROPERTIES LABELS "unit;shm_ring_buffer;membership")

add_executable(process_shared_sync_backpressure_test process_shared_sync_backpressure_test.cpp)
target_link_libraries(
  process_shared_sync_backpressure_test
  PRIVATE
    comm_core
    common_build_flags
    GTest::gtest_main
)

gtest_discover_tests(
  process_shared_sync_backpressure_test
  PROPERTIES LABELS "unit;sync;backpressure;shm_pubsub"
)

add_executable(split_shm_bus_control_runtime_test split_shm_bus_control_runtime_test.cpp)
target_link_libraries(
  split_shm_bus_control_runtime_test
  PRIVATE
    comm_core
    config_core
    common_build_flags
    GTest::gtest_main
)
gtest_discover_tests(
  split_shm_bus_control_runtime_test
  PROPERTIES LABELS "unit;shm_pubsub;control_plane"
)

add_executable(mouldBase_test mouldBase_test.cpp)
target_link_libraries(
  mouldBase_test
  PRIVATE
    comm_core
    common_build_flags
    GTest::gtest_main
)
gtest_discover_tests(
  mouldBase_test
  PROPERTIES LABELS "unit;module_base"
)

add_executable(supervisor_test supervisor_test.cpp)
target_link_libraries(
  supervisor_test
  PRIVATE
    comm_core
    common_build_flags
    GTest::gtest_main
)
gtest_discover_tests(
  supervisor_test
  PROPERTIES LABELS "unit;supervisor"
)

add_executable(module_factory_registry_test module_factory_registry_test.cpp)
target_link_libraries(
  module_factory_registry_test
  PRIVATE
    comm_core
    common_build_flags
    GTest::gtest_main
)
gtest_discover_tests(
  module_factory_registry_test
  PROPERTIES LABELS "unit;module_factory_registry"
)
