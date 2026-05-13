# HTTP 层 — 后续任务添加源文件后 gate_http 自动变为 OBJECT 库
file(GLOB _http_src *.cpp)
if(_http_src)
  add_library(gate_http STATIC ${_http_src})
  target_link_libraries(gate_http PUBLIC common_build_flags Moduo::Moduo)
  target_include_directories(gate_http PUBLIC "${CMAKE_SOURCE_DIR}")
endif()
