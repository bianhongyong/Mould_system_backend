# Module 层 — http_gateway_module.cpp 编译为 gate_module OBJECT 库
# 链接所有 gate_* 子库及 comm_core（ModuleBase、模块注册宏）
file(GLOB _module_src *.cpp)
list(APPEND _module_src "${CMAKE_CURRENT_SOURCE_DIR}/../gflags/gateway_module_gflags.cpp")
if(_module_src)
  add_library(gate_module OBJECT ${_module_src})
  target_link_libraries(gate_module PUBLIC common_build_flags)
  target_link_libraries(gate_module PUBLIC gate_handler gate_http gate_oss comm_core)
  target_include_directories(gate_module PUBLIC "${CMAKE_SOURCE_DIR}")
endif()
