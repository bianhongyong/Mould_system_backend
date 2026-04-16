find_path(MySQLClient_INCLUDE_DIR
  NAMES mysql/mysql.h
  PATH_SUFFIXES include
)

find_library(MySQLClient_LIBRARY
  NAMES mysqlclient mariadb
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(MySQLClient DEFAULT_MSG MySQLClient_INCLUDE_DIR MySQLClient_LIBRARY)

if(MySQLClient_FOUND)
  set(MySQLClient_INCLUDE_DIRS "${MySQLClient_INCLUDE_DIR}")
  set(MySQLClient_LIBRARIES "${MySQLClient_LIBRARY}")

  if(NOT TARGET MySQLClient::MySQLClient)
    add_library(MySQLClient::MySQLClient UNKNOWN IMPORTED)
    set_target_properties(MySQLClient::MySQLClient PROPERTIES
      IMPORTED_LOCATION "${MySQLClient_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${MySQLClient_INCLUDE_DIR}"
    )
  endif()
endif()

mark_as_advanced(MySQLClient_INCLUDE_DIR MySQLClient_LIBRARY)
