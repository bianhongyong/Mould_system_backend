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
add_subdirectory(logging)
add_subdirectory(services/backend)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/services/broker/CMakeLists.txt")
  add_subdirectory(services/broker)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/services/infer/CMakeLists.txt")
  add_subdirectory(services/infer)
endif()
add_subdirectory(tests)

add_custom_target(
  backend_all
  DEPENDS
    backend_main
    third_party_probe
)
if(TARGET broker_all)
  add_dependencies(backend_all broker_all)
endif()
if(TARGET infer_all)
  add_dependencies(backend_all infer_all)
endif()
