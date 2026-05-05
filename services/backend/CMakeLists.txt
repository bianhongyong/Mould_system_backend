add_executable(
  backend_main
  main.cpp
  modules/sample_modules.cpp
  test/sensor_fusion_module.cpp
  test/risk_eval_module.cpp
  test/path_plan_module.cpp
  test/actuator_coord_module.cpp
  test/compete_stress_modules.cpp
)

target_link_libraries(
  backend_main
  PRIVATE
    comm_core
    config_core
    common_build_flags
)
