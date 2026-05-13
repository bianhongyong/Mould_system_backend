# Handler 层 — 包含 protobuf 序列化的帧载荷编译
file(GLOB _handler_src *.cpp)
if(_handler_src)
  add_library(gate_handler OBJECT ${_handler_src})
  # 依赖 comm_core：保证 gateway_frame_ingress.pb.h 等由 comm 侧 protoc 先生成，并传播生成目录 include
  target_link_libraries(gate_handler PUBLIC common_build_flags protobuf::libprotobuf comm_core)
  target_include_directories(gate_handler PUBLIC "${CMAKE_SOURCE_DIR}")
endif()
