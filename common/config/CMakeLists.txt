add_library(
  config_core
  src/channel_topology_config.cpp
)

target_include_directories(config_core PUBLIC include)
target_link_libraries(config_core PUBLIC common_build_flags)
