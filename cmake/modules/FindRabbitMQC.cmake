find_path(RabbitMQC_INCLUDE_DIR
  NAMES amqp.h
  PATH_SUFFIXES include
)

find_library(RabbitMQC_LIBRARY
  NAMES rabbitmq rabbitmqc
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(RabbitMQC DEFAULT_MSG RabbitMQC_INCLUDE_DIR RabbitMQC_LIBRARY)

if(RabbitMQC_FOUND)
  set(RabbitMQC_INCLUDE_DIRS "${RabbitMQC_INCLUDE_DIR}")
  set(RabbitMQC_LIBRARIES "${RabbitMQC_LIBRARY}")

  if(NOT TARGET RabbitMQC::RabbitMQC)
    add_library(RabbitMQC::RabbitMQC UNKNOWN IMPORTED)
    set_target_properties(RabbitMQC::RabbitMQC PROPERTIES
      IMPORTED_LOCATION "${RabbitMQC_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${RabbitMQC_INCLUDE_DIR}"
    )
  endif()
endif()

mark_as_advanced(RabbitMQC_INCLUDE_DIR RabbitMQC_LIBRARY)
