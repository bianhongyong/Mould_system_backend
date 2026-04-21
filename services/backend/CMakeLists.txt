add_executable(
  backend_main
  main.cpp
  modules/sample_modules.cpp
)

target_link_libraries(
  backend_main
  PRIVATE
    comm_core
    config_core
    common_build_flags
)
