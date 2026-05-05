if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/infrastructure/third_party_probe.cpp")
  add_executable(third_party_probe infrastructure/third_party_probe.cpp)
  target_link_libraries(third_party_probe PRIVATE common_build_flags)

  add_test(NAME third_party_probe_self_test COMMAND third_party_probe)
  set_tests_properties(third_party_probe_self_test PROPERTIES LABELS "third-party;probe")
endif()

if(TARGET broker_infer_e2e)
  add_test(NAME broker_infer_e2e_test COMMAND broker_infer_e2e)
  set_tests_properties(broker_infer_e2e_test PROPERTIES LABELS "middleware;e2e")
endif()

if(TARGET backend_main)
  if(EXISTS "${CMAKE_SOURCE_DIR}/tests/infrastructure/backend_main_smoke.sh")
    add_test(
      NAME backend_main_smoke_test
      COMMAND
        bash
        "${CMAKE_SOURCE_DIR}/tests/infrastructure/backend_main_smoke.sh"
        "$<TARGET_FILE:backend_main>"
        "${CMAKE_SOURCE_DIR}/launch_plan.json"
    )
    set_tests_properties(
      backend_main_smoke_test
      PROPERTIES
        LABELS "smoke;backend_main"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        TIMEOUT 10
    )
  endif()
endif()

find_package(GTest REQUIRED)
include(GoogleTest)

set(PROTO_RESOURCE_ROOT "${CMAKE_SOURCE_DIR}/tests/resources/proto")
set(PROTO_GEN_ROOT "${CMAKE_BINARY_DIR}/proto")

set(UNIFIED_PROTO_INPUTS
  "${CMAKE_SOURCE_DIR}/common/proto/image_frame.proto"
  "${PROTO_RESOURCE_ROOT}/sensor_fusion/sensor_fusion_telemetry.proto"
  "${PROTO_RESOURCE_ROOT}/risk_eval/risk_eval_core.proto"
  "${PROTO_RESOURCE_ROOT}/path_plan/path_plan_core.proto"
  "${PROTO_RESOURCE_ROOT}/actuator_coord/actuator_coord_core.proto"
  "${PROTO_RESOURCE_ROOT}/unified_output_envelope.proto"
)

set(UNIFIED_PROTO_GENERATED_SRCS
  "${PROTO_GEN_ROOT}/image_frame.pb.cc"
  "${PROTO_GEN_ROOT}/sensor_fusion/sensor_fusion_telemetry.pb.cc"
  "${PROTO_GEN_ROOT}/risk_eval/risk_eval_core.pb.cc"
  "${PROTO_GEN_ROOT}/path_plan/path_plan_core.pb.cc"
  "${PROTO_GEN_ROOT}/actuator_coord/actuator_coord_core.pb.cc"
  "${PROTO_GEN_ROOT}/unified_output_envelope.pb.cc"
)

set(UNIFIED_PROTO_GENERATED_HDRS
  "${PROTO_GEN_ROOT}/image_frame.pb.h"
  "${PROTO_GEN_ROOT}/sensor_fusion/sensor_fusion_telemetry.pb.h"
  "${PROTO_GEN_ROOT}/risk_eval/risk_eval_core.pb.h"
  "${PROTO_GEN_ROOT}/path_plan/path_plan_core.pb.h"
  "${PROTO_GEN_ROOT}/actuator_coord/actuator_coord_core.pb.h"
  "${PROTO_GEN_ROOT}/unified_output_envelope.pb.h"
)

if(TARGET protobuf::protoc)
  set(MOULD_PROTOC_EXECUTABLE $<TARGET_FILE:protobuf::protoc>)
elseif(DEFINED Protobuf_PROTOC_EXECUTABLE)
  set(MOULD_PROTOC_EXECUTABLE "${Protobuf_PROTOC_EXECUTABLE}")
else()
  find_program(MOULD_PROTOC_EXECUTABLE protoc REQUIRED)
endif()

add_custom_command(
  OUTPUT ${UNIFIED_PROTO_GENERATED_SRCS} ${UNIFIED_PROTO_GENERATED_HDRS}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${PROTO_GEN_ROOT}
  COMMAND ${MOULD_PROTOC_EXECUTABLE}
          --proto_path=${CMAKE_SOURCE_DIR}/common/proto
          --proto_path=${PROTO_RESOURCE_ROOT}
          --cpp_out=${PROTO_GEN_ROOT}
          ${CMAKE_SOURCE_DIR}/common/proto/image_frame.proto
          ${PROTO_RESOURCE_ROOT}/sensor_fusion/sensor_fusion_telemetry.proto
          ${PROTO_RESOURCE_ROOT}/risk_eval/risk_eval_core.proto
          ${PROTO_RESOURCE_ROOT}/path_plan/path_plan_core.proto
          ${PROTO_RESOURCE_ROOT}/actuator_coord/actuator_coord_core.proto
          ${PROTO_RESOURCE_ROOT}/unified_output_envelope.proto
  DEPENDS ${UNIFIED_PROTO_INPUTS}
  COMMENT "Generating unified proto sources under ${PROTO_GEN_ROOT}"
  VERBATIM
)

add_library(unified_output_proto_generated STATIC ${UNIFIED_PROTO_GENERATED_SRCS})
target_include_directories(
  unified_output_proto_generated
  PUBLIC
    "${PROTO_GEN_ROOT}"
    "${PROTO_RESOURCE_ROOT}"
)
target_link_libraries(unified_output_proto_generated PUBLIC common_build_flags)

add_executable(
  unified_output_envelope_gtest
  "${CMAKE_SOURCE_DIR}/tests/resources/proto/unified_output_envelope_gtest.cpp"
)
target_link_libraries(
  unified_output_envelope_gtest
  PRIVATE
    unified_output_proto_generated
    common_build_flags
    GTest::gtest_main
)
gtest_discover_tests(
  unified_output_envelope_gtest
  PROPERTIES LABELS "unit;proto;unified_output_envelope"
)
