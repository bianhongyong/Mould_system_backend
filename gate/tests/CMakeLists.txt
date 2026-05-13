find_package(GTest REQUIRED)
include(GoogleTest)

# 自动发现测试源文件，每个 *_test.cpp 编译为一个独立测试目标
file(GLOB _gate_test_srcs *_test.cpp)
foreach(_test_src ${_gate_test_srcs})
  get_filename_component(_test_name ${_test_src} NAME_WE)
  add_executable(${_test_name} ${_test_src})
  target_link_libraries(${_test_name}
    PRIVATE
      gate_core
      gate_handler
      comm_core
      logging_core
      GTest::gtest
      GTest::gmock
  )
  if(TARGET gate_oss)
    target_link_libraries(${_test_name} PRIVATE gate_oss)
    if(TARGET OSSSDK::OSSSDK)
      find_package(OpenSSL REQUIRED)
      find_package(CURL REQUIRED)
      # 用 --start-group/--end-group 包裹 OSS SDK 及其传递依赖，
      # 确保静态库之间符号可交叉解析
      target_link_options(${_test_name} PRIVATE "LINKER:--push-state,--as-needed")
      target_link_options(${_test_name} PRIVATE "LINKER:--start-group")
      target_link_libraries(${_test_name} PRIVATE OSSSDK::OSSSDK OpenSSL::SSL OpenSSL::Crypto CURL::libcurl)
      target_link_options(${_test_name} PRIVATE "LINKER:--end-group")
      target_link_options(${_test_name} PRIVATE "LINKER:--pop-state")
    endif()
  endif()
  # 如果 gate_module 存在（有 module 源文件），将其 object 文件链接进来
  if(TARGET gate_module)
    target_link_libraries(${_test_name} PRIVATE gate_module)
  endif()
  add_test(NAME ${_test_name} COMMAND ${_test_name})
  set_tests_properties(${_test_name} PROPERTIES LABELS "unit;gate")
endforeach()
