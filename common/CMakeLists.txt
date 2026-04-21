cmake_minimum_required(VERSION 3.22)
project(mould_common LANGUAGES CXX)

include(CTest)
find_package(Threads REQUIRED)

# 允许 common 子工程独立配置：若顶层未注入 thirdparty::deps，则在本地提供空聚合目标。
if(NOT TARGET thirdparty::deps)
  add_library(thirdparty_deps INTERFACE)
  add_library(thirdparty::deps ALIAS thirdparty_deps)
  find_package(gflags QUIET CONFIG)
  find_package(glog QUIET CONFIG)
  if(TARGET gflags::gflags)
    target_link_libraries(thirdparty_deps INTERFACE gflags::gflags)
  elseif(TARGET gflags)
    target_link_libraries(thirdparty_deps INTERFACE gflags)
  endif()
  if(TARGET glog::glog)
    target_link_libraries(thirdparty_deps INTERFACE glog::glog)
  elseif(TARGET glog)
    target_link_libraries(thirdparty_deps INTERFACE glog)
  endif()
endif()

add_library(common_build_flags INTERFACE)

target_compile_features(common_build_flags INTERFACE cxx_std_20)
target_link_libraries(common_build_flags INTERFACE thirdparty::deps)
target_include_directories(common_build_flags INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_subdirectory(config)
add_subdirectory(comm)

add_executable(channel_topology_config_test config/channel_topology_config_test.cpp)
target_link_libraries(channel_topology_config_test PRIVATE comm_core config_core common_build_flags)
add_test(NAME channel_topology_config_test COMMAND channel_topology_config_test)
set_tests_properties(channel_topology_config_test PROPERTIES LABELS "unit;channel_topology_config")
