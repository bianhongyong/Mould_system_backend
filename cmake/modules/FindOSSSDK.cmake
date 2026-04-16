find_path(OSSSDK_INCLUDE_DIR
  NAMES alibabacloud/oss/OssClient.h
  PATH_SUFFIXES include
)

find_library(OSSSDK_LIBRARY
  NAMES alibabacloud-oss-cpp-sdk oss-cpp-sdk
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(OSSSDK DEFAULT_MSG OSSSDK_INCLUDE_DIR OSSSDK_LIBRARY)

if(OSSSDK_FOUND)
  set(OSSSDK_INCLUDE_DIRS "${OSSSDK_INCLUDE_DIR}")
  set(OSSSDK_LIBRARIES "${OSSSDK_LIBRARY}")

  if(NOT TARGET OSSSDK::OSSSDK)
    add_library(OSSSDK::OSSSDK UNKNOWN IMPORTED)
    set_target_properties(OSSSDK::OSSSDK PROPERTIES
      IMPORTED_LOCATION "${OSSSDK_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${OSSSDK_INCLUDE_DIR}"
    )
  endif()
endif()

mark_as_advanced(OSSSDK_INCLUDE_DIR OSSSDK_LIBRARY)
