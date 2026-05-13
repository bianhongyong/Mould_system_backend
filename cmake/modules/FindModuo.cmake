find_path(Moduo_INCLUDE_DIR
  NAMES muduo/base/Types.h muduo/base/Timestamp.h
  PATH_SUFFIXES include
)

find_library(Moduo_BASE_LIBRARY
  NAMES muduo_base
)

find_library(Moduo_NET_LIBRARY
  NAMES muduo_net
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Moduo DEFAULT_MSG Moduo_INCLUDE_DIR
  Moduo_BASE_LIBRARY Moduo_NET_LIBRARY)

if(Moduo_FOUND)
  set(Moduo_INCLUDE_DIRS "${Moduo_INCLUDE_DIR}")
  set(Moduo_LIBRARIES "${Moduo_BASE_LIBRARY}" "${Moduo_NET_LIBRARY}")

  if(NOT TARGET Moduo::Moduo)
    add_library(Moduo::Moduo INTERFACE IMPORTED)
    set_target_properties(Moduo::Moduo PROPERTIES
      INTERFACE_INCLUDE_DIRECTORIES "${Moduo_INCLUDE_DIR}"
      INTERFACE_LINK_LIBRARIES "${Moduo_NET_LIBRARY};${Moduo_BASE_LIBRARY}"
    )
  endif()
endif()

mark_as_advanced(Moduo_INCLUDE_DIR Moduo_LIBRARY)
