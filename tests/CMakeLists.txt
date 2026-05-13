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

add_executable(
  unified_output_envelope_gtest
  "${CMAKE_SOURCE_DIR}/tests/resources/proto/unified_output_envelope_gtest.cpp"
)
target_link_libraries(
  unified_output_envelope_gtest
  PRIVATE
    comm_core
    common_build_flags
    GTest::gtest_main
)
gtest_discover_tests(
  unified_output_envelope_gtest
  PROPERTIES LABELS "unit;proto;unified_output_envelope"
)
