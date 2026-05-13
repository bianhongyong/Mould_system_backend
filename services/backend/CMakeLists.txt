add_executable(
  backend_main
  main.cpp
  modules/sample_modules.cpp
  test/sensor_fusion_module.cpp
  test/risk_eval_module.cpp
  test/path_plan_module.cpp
  test/actuator_coord_module.cpp
  test/compete_stress_modules.cpp
  test/http_frame_sink_module.cpp
)

target_include_directories(
  backend_main
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(
  backend_main
  PRIVATE
    comm_core
    logging_core
    config_core
    common_build_flags
    gate_core
    gate_module
    gate_handler
)

# gate_module -> gate_oss (static) -> OSSSDK (static).
# 后者内部引用 OpenSSL/CURL，但 libssl.so 在链接命令行中的位置
# 早于 libalibabacloud-oss-cpp-sdk.a，默认 --as-needed 会因为没有
# 提前的 EVP_* 引用而跳过 libssl.so。最后用 --no-as-needed 重链
# OpenSSL/CURL，确保其符号全局可解析。
if(TARGET OSSSDK::OSSSDK)
  find_package(OpenSSL REQUIRED)
  find_package(CURL REQUIRED)
  target_link_libraries(backend_main PRIVATE
    "-Wl,--no-as-needed"
    OpenSSL::SSL
    OpenSSL::Crypto
    CURL::libcurl
    "-Wl,--as-needed"
  )
endif()