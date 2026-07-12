find_path(Libevent_INCLUDE_DIR
  NAMES event2/event.h
)
find_library(Libevent_CORE_LIBRARY
  NAMES event_core event
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Libevent
  REQUIRED_VARS Libevent_CORE_LIBRARY Libevent_INCLUDE_DIR
)

if(Libevent_FOUND AND NOT TARGET Libevent::core)
  add_library(Libevent::core UNKNOWN IMPORTED)
  set_target_properties(Libevent::core PROPERTIES
    IMPORTED_LOCATION "${Libevent_CORE_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${Libevent_INCLUDE_DIR}"
  )
endif()

mark_as_advanced(Libevent_INCLUDE_DIR Libevent_CORE_LIBRARY)
