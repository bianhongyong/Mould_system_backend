set(MOULD_COMM_PROTO_RESOURCE_ROOT "${CMAKE_SOURCE_DIR}/tests/resources/proto")
set(MOULD_COMM_COMMON_PROTO_ROOT "${CMAKE_SOURCE_DIR}/common/proto")
set(MOULD_COMM_PROTO_GEN_ROOT "${CMAKE_BINARY_DIR}/proto")

set(MOULD_COMM_PROTO_INPUTS
  "${MOULD_COMM_COMMON_PROTO_ROOT}/image_frame.proto"
  "${MOULD_COMM_PROTO_RESOURCE_ROOT}/sensor_fusion/sensor_fusion_telemetry.proto"
  "${MOULD_COMM_PROTO_RESOURCE_ROOT}/risk_eval/risk_eval_core.proto"
  "${MOULD_COMM_PROTO_RESOURCE_ROOT}/path_plan/path_plan_core.proto"
  "${MOULD_COMM_PROTO_RESOURCE_ROOT}/actuator_coord/actuator_coord_core.proto"
  "${MOULD_COMM_PROTO_RESOURCE_ROOT}/unified_output_envelope.proto"
)

set(MOULD_COMM_PROTO_GENERATED_SRCS
  "${MOULD_COMM_PROTO_GEN_ROOT}/image_frame.pb.cc"
  "${MOULD_COMM_PROTO_GEN_ROOT}/sensor_fusion/sensor_fusion_telemetry.pb.cc"
  "${MOULD_COMM_PROTO_GEN_ROOT}/risk_eval/risk_eval_core.pb.cc"
  "${MOULD_COMM_PROTO_GEN_ROOT}/path_plan/path_plan_core.pb.cc"
  "${MOULD_COMM_PROTO_GEN_ROOT}/actuator_coord/actuator_coord_core.pb.cc"
  "${MOULD_COMM_PROTO_GEN_ROOT}/unified_output_envelope.pb.cc"
)

set(MOULD_COMM_PROTO_GENERATED_HDRS
  "${MOULD_COMM_PROTO_GEN_ROOT}/image_frame.pb.h"
  "${MOULD_COMM_PROTO_GEN_ROOT}/sensor_fusion/sensor_fusion_telemetry.pb.h"
  "${MOULD_COMM_PROTO_GEN_ROOT}/risk_eval/risk_eval_core.pb.h"
  "${MOULD_COMM_PROTO_GEN_ROOT}/path_plan/path_plan_core.pb.h"
  "${MOULD_COMM_PROTO_GEN_ROOT}/actuator_coord/actuator_coord_core.pb.h"
  "${MOULD_COMM_PROTO_GEN_ROOT}/unified_output_envelope.pb.h"
)

if(TARGET protobuf::protoc)
  set(MOULD_COMM_PROTOC_EXECUTABLE $<TARGET_FILE:protobuf::protoc>)
elseif(DEFINED Protobuf_PROTOC_EXECUTABLE)
  set(MOULD_COMM_PROTOC_EXECUTABLE "${Protobuf_PROTOC_EXECUTABLE}")
else()
  find_program(MOULD_COMM_PROTOC_EXECUTABLE protoc REQUIRED)
endif()

add_custom_command(
  OUTPUT ${MOULD_COMM_PROTO_GENERATED_SRCS} ${MOULD_COMM_PROTO_GENERATED_HDRS}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${MOULD_COMM_PROTO_GEN_ROOT}
  COMMAND ${MOULD_COMM_PROTOC_EXECUTABLE}
          --proto_path=${MOULD_COMM_COMMON_PROTO_ROOT}
          --proto_path=${MOULD_COMM_PROTO_RESOURCE_ROOT}
          --cpp_out=${MOULD_COMM_PROTO_GEN_ROOT}
          ${MOULD_COMM_COMMON_PROTO_ROOT}/image_frame.proto
          ${MOULD_COMM_PROTO_RESOURCE_ROOT}/sensor_fusion/sensor_fusion_telemetry.proto
          ${MOULD_COMM_PROTO_RESOURCE_ROOT}/risk_eval/risk_eval_core.proto
          ${MOULD_COMM_PROTO_RESOURCE_ROOT}/path_plan/path_plan_core.proto
          ${MOULD_COMM_PROTO_RESOURCE_ROOT}/actuator_coord/actuator_coord_core.proto
          ${MOULD_COMM_PROTO_RESOURCE_ROOT}/unified_output_envelope.proto
  DEPENDS ${MOULD_COMM_PROTO_INPUTS}
  COMMENT "Generating comm proto sources under ${MOULD_COMM_PROTO_GEN_ROOT}"
  VERBATIM
)

add_library(
  comm_core
  src/callback_queue.cpp
  src/channel_factory.cpp
  src/image_pipeline.cpp
  src/module_factory_registry.cpp
  src/module_base.cpp
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
  ${MOULD_COMM_PROTO_GENERATED_SRCS}
)

target_include_directories(
  comm_core
  PUBLIC
    include
    "${MOULD_COMM_PROTO_GEN_ROOT}"
    "${MOULD_COMM_PROTO_RESOURCE_ROOT}"
)
target_link_libraries(comm_core PUBLIC common_build_flags config_core)
if(TARGET protobuf::libprotobuf)
  target_link_libraries(comm_core PUBLIC protobuf::libprotobuf)
endif()
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
target_link_libraries(
  multi_consumer_cursor_reclaim_test
  PRIVATE
    comm_core
    common_build_flags
    GTest::gtest
)
gtest_discover_tests(
  multi_consumer_cursor_reclaim_test
  PROPERTIES LABELS "unit;shm_pubsub;ring"
)

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

