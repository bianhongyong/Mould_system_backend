add_library(
  comm_core
  src/callback_queue.cpp
  src/channel_factory.cpp
  src/image_pipeline.cpp
  src/module_base.cpp
  src/payload_codec.cpp
  src/refcounted_message_store.cpp
  src/reliability.cpp
  src/shm_ring_buffer.cpp
  src/shm_segment.cpp
  src/shm_pubsub_bus.cpp
)

target_include_directories(comm_core PUBLIC include)
target_link_libraries(comm_core PUBLIC common_build_flags config_core)

add_executable(shm_segment_test shm_segment_test.cpp)
target_link_libraries(shm_segment_test PRIVATE comm_core common_build_flags)
add_test(NAME shm_segment_test COMMAND shm_segment_test)
set_tests_properties(shm_segment_test PROPERTIES LABELS "unit;shm_segment")

add_executable(shm_ring_buffer_layout_test shm_ring_buffer_layout_test.cpp)
target_link_libraries(shm_ring_buffer_layout_test PRIVATE comm_core common_build_flags)
add_test(NAME shm_ring_buffer_layout_test COMMAND shm_ring_buffer_layout_test)
set_tests_properties(shm_ring_buffer_layout_test PROPERTIES LABELS "unit;shm_ring_buffer")

add_executable(multi_consumer_cursor_reclaim_test multi_consumer_cursor_reclaim_test.cpp)
target_link_libraries(multi_consumer_cursor_reclaim_test PRIVATE comm_core common_build_flags)
add_test(NAME multi_consumer_cursor_reclaim_test COMMAND multi_consumer_cursor_reclaim_test)
set_tests_properties(multi_consumer_cursor_reclaim_test PROPERTIES LABELS "unit;shm_pubsub;ring")

add_executable(daemon_membership_observability_test daemon_membership_observability_test.cpp)
target_link_libraries(daemon_membership_observability_test PRIVATE comm_core common_build_flags)
add_test(NAME daemon_membership_observability_test COMMAND daemon_membership_observability_test)
set_tests_properties(daemon_membership_observability_test PROPERTIES LABELS "unit;shm_ring_buffer;membership")

find_package(GTest REQUIRED)
include(GoogleTest)

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
