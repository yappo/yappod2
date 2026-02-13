cmake_minimum_required(VERSION 3.20)
project(yappod C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

enable_testing()

option(YAPPOD_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)

if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  add_compile_options(-Wall -Wextra -Wpedantic)
  if(YAPPOD_WARNINGS_AS_ERRORS)
    add_compile_options(-Werror)
  endif()
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_compile_definitions(_DEFAULT_SOURCE _POSIX_C_SOURCE=200809L)
endif()

find_package(Threads REQUIRED)
find_package(ZLIB REQUIRED)

find_path(BDB_INCLUDE_DIR db.h)
find_library(BDB_LIB db)
if(NOT BDB_INCLUDE_DIR OR NOT BDB_LIB)
  message(FATAL_ERROR "Berkeley DB not found. Install berkeley-db and set BDB_INCLUDE_DIR/BDB_LIB if needed.")
endif()

set(SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)

set(YAPPO_BASE_SOURCES
  ${SRC_DIR}/yappo_alloc.c
  ${SRC_DIR}/yappo_io.c
  ${SRC_DIR}/yappo_net.c
  ${SRC_DIR}/yappo_stat.c
  ${SRC_DIR}/yappo_db.c
  ${SRC_DIR}/yappo_index.c
  ${SRC_DIR}/yappo_index_pos.c
  ${SRC_DIR}/yappo_index_filedata.c
  ${SRC_DIR}/yappo_index_deletefile.c
)

set(YAPPO_SEARCH_SOURCES
  ${SRC_DIR}/yappo_search.c
  ${SRC_DIR}/yappo_ngram.c
  ${SRC_DIR}/yappo_linklist.c
  ${SRC_DIR}/yappo_proto.c
)

add_executable(search
  ${YAPPO_BASE_SOURCES}
  ${YAPPO_SEARCH_SOURCES}
  ${SRC_DIR}/search.c
)

add_executable(yappo_makeindex
  ${YAPPO_BASE_SOURCES}
  ${SRC_DIR}/yappo_ngram.c
  ${SRC_DIR}/yappo_minibtree.c
  ${SRC_DIR}/yappo_makeindex.c
)

add_executable(yappo_mergepos
  ${YAPPO_BASE_SOURCES}
  ${YAPPO_SEARCH_SOURCES}
  ${SRC_DIR}/yappo_mergepos.c
)

add_executable(yappod_core
  ${YAPPO_BASE_SOURCES}
  ${YAPPO_SEARCH_SOURCES}
  ${SRC_DIR}/yappod_core.c
)

add_executable(yappod_front
  ${YAPPO_BASE_SOURCES}
  ${YAPPO_SEARCH_SOURCES}
  ${SRC_DIR}/yappod_front.c
)

add_executable(search_deletefile_bounds_test
  ${YAPPO_BASE_SOURCES}
  ${YAPPO_SEARCH_SOURCES}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/search_deletefile_bounds_test.c
)

set(ALL_TARGETS
  search
  yappo_makeindex
  yappo_mergepos
  yappod_core
  yappod_front
)

set(LINK_TARGETS
  ${ALL_TARGETS}
  search_deletefile_bounds_test
)

foreach(tgt IN LISTS LINK_TARGETS)
  target_include_directories(${tgt} PRIVATE ${SRC_DIR} ${BDB_INCLUDE_DIR})
  target_link_libraries(${tgt} PRIVATE ${BDB_LIB} ZLIB::ZLIB Threads::Threads)
endforeach()

find_library(M_LIB m)
if(M_LIB)
  foreach(tgt IN LISTS LINK_TARGETS)
    target_link_libraries(${tgt} PRIVATE ${M_LIB})
  endforeach()
endif()

install(TARGETS ${ALL_TARGETS}
  RUNTIME DESTINATION bin
)

add_test(NAME e2e COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/e2e.sh)
add_test(NAME error_cases COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/error_cases.sh)
add_test(NAME mergepos_error_cases COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/mergepos_error_cases.sh)
add_test(NAME opendir_error_cases COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/opendir_error_cases.sh)
add_test(NAME relative_index_dir_cases COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/relative_index_dir_cases.sh)
add_test(NAME search_deletefile_bounds COMMAND $<TARGET_FILE:search_deletefile_bounds_test>)
