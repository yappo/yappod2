find_path(CMOCKA_INCLUDE_DIR cmocka.h)
find_library(CMOCKA_LIBRARY NAMES cmocka)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(cmocka
  REQUIRED_VARS CMOCKA_LIBRARY CMOCKA_INCLUDE_DIR
)

if(cmocka_FOUND)
  set(CMOCKA_LIBRARIES ${CMOCKA_LIBRARY})
  set(CMOCKA_INCLUDE_DIRS ${CMOCKA_INCLUDE_DIR})

  if(NOT TARGET cmocka::cmocka)
    add_library(cmocka::cmocka UNKNOWN IMPORTED)
    set_target_properties(cmocka::cmocka PROPERTIES
      IMPORTED_LOCATION "${CMOCKA_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${CMOCKA_INCLUDE_DIR}"
    )
  endif()
endif()

mark_as_advanced(CMOCKA_INCLUDE_DIR CMOCKA_LIBRARY)
