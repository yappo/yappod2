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

add_executable(yappo_margepos
  ${YAPPO_BASE_SOURCES}
  ${YAPPO_SEARCH_SOURCES}
  ${SRC_DIR}/yappo_margepos.c
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

set(ALL_TARGETS
  search
  yappo_makeindex
  yappo_margepos
  yappod_core
  yappod_front
)

foreach(tgt IN LISTS ALL_TARGETS)
  target_include_directories(${tgt} PRIVATE ${SRC_DIR} ${BDB_INCLUDE_DIR})
  target_link_libraries(${tgt} PRIVATE ${BDB_LIB} ZLIB::ZLIB Threads::Threads)
endforeach()

find_library(M_LIB m)
if(M_LIB)
  foreach(tgt IN LISTS ALL_TARGETS)
    target_link_libraries(${tgt} PRIVATE ${M_LIB})
  endforeach()
endif()

install(TARGETS ${ALL_TARGETS}
  RUNTIME DESTINATION bin
)

add_test(NAME e2e COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/e2e.sh)
