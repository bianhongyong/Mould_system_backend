add_executable(third_party_probe infrastructure/third_party_probe.cpp)
target_link_libraries(third_party_probe PRIVATE common_build_flags)

add_test(NAME third_party_probe_self_test COMMAND third_party_probe)
set_tests_properties(third_party_probe_self_test PROPERTIES LABELS "third-party;probe")

if(TARGET broker_infer_e2e)
  add_test(NAME broker_infer_e2e_test COMMAND broker_infer_e2e)
  set_tests_properties(broker_infer_e2e_test PROPERTIES LABELS "middleware;e2e")
endif()

if(TARGET backend_main)
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
