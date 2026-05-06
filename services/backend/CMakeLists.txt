add_executable(
  backend_main
  main.cpp
  modules/sample_modules.cpp
  modules/logging_module.cpp
  gflags/logging_module_gflags.cpp
  test/sensor_fusion_module.cpp
  test/risk_eval_module.cpp
  test/path_plan_module.cpp
  test/actuator_coord_module.cpp
  test/compete_stress_modules.cpp
)

target_include_directories(
  backend_main
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(
  backend_main
  PRIVATE
    logging_core
    comm_core
    config_core
    common_build_flags
)
