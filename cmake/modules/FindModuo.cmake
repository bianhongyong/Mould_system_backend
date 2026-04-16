find_path(Moduo_INCLUDE_DIR
  NAMES muduo/base/Types.h muduo/base/Timestamp.h
  PATH_SUFFIXES include
)

find_library(Moduo_LIBRARY
  NAMES muduo_base moduo muduo
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Moduo DEFAULT_MSG Moduo_INCLUDE_DIR Moduo_LIBRARY)

if(Moduo_FOUND)
  set(Moduo_INCLUDE_DIRS "${Moduo_INCLUDE_DIR}")
  set(Moduo_LIBRARIES "${Moduo_LIBRARY}")

  if(NOT TARGET Moduo::Moduo)
    add_library(Moduo::Moduo UNKNOWN IMPORTED)
    set_target_properties(Moduo::Moduo PROPERTIES
      IMPORTED_LOCATION "${Moduo_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${Moduo_INCLUDE_DIR}"
    )
  endif()
endif()

mark_as_advanced(Moduo_INCLUDE_DIR Moduo_LIBRARY)
