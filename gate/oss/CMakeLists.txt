# OSS 封装层 — OssClient 基于阿里云 OSS SDK，为硬依赖

file(GLOB _oss_src *.cpp)

if(_oss_src)
  add_library(gate_oss STATIC ${_oss_src})
  target_link_libraries(gate_oss PUBLIC common_build_flags)
  target_include_directories(
    gate_oss
    PUBLIC
      "${CMAKE_CURRENT_SOURCE_DIR}"
  )

  find_package(OSSSDK REQUIRED)
  find_package(OpenSSL REQUIRED)
  find_package(CURL REQUIRED)
  target_link_libraries(gate_oss PUBLIC OSSSDK::OSSSDK)
  target_link_libraries(gate_oss PUBLIC OpenSSL::SSL OpenSSL::Crypto)
  target_link_libraries(gate_oss PUBLIC CURL::libcurl)

  message(STATUS "[gate_oss] OSS SDK enabled")
endif()
