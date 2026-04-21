cmake_minimum_required(VERSION 3.22)

project(
  mould_system_backend
  VERSION 0.1.0
  LANGUAGES CXX
)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules")

include(cmake/platform.cmake)
include(cmake/third_party.cmake)

enable_testing()

add_subdirectory(common)
add_subdirectory(services/backend)
add_subdirectory(services/broker)
add_subdirectory(services/infer)
add_subdirectory(tests)

add_custom_target(
  backend_all
  DEPENDS
    backend_main
    broker_all
    infer_all
    third_party_probe
)
