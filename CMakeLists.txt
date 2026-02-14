cmake_minimum_required(VERSION 3.20)
project(yappod C)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

enable_testing()

option(YAPPOD_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(YAPPOD_TESTS_DAEMON "Enable daemon-based test suites" ON)
option(YAPPOD_TESTS_NODAEMON "Enable non-daemon test suites" ON)

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
find_package(cmocka REQUIRED)

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

set(ALL_TARGETS
  search
  yappo_makeindex
  yappo_mergepos
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

set(TEST_COMMON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmocka/common)

set(TEST_SUPPORT_SOURCES
  ${TEST_COMMON_DIR}/test_env.c
  ${TEST_COMMON_DIR}/test_fs.c
  ${TEST_COMMON_DIR}/test_cli.c
  ${TEST_COMMON_DIR}/test_http.c
  ${TEST_COMMON_DIR}/test_index.c
  ${TEST_COMMON_DIR}/test_input_builder.c
  ${TEST_COMMON_DIR}/test_proc.c
  ${TEST_COMMON_DIR}/test_db_tmpfiles.c
  ${TEST_COMMON_DIR}/test_yappod.c
  ${TEST_COMMON_DIR}/test_mutation.c
)

add_library(yappod_test_support STATIC ${TEST_SUPPORT_SOURCES})
target_include_directories(yappod_test_support
  PUBLIC ${SRC_DIR} ${TEST_COMMON_DIR} ${BDB_INCLUDE_DIR}
)
target_compile_definitions(yappod_test_support
  PRIVATE
    YTEST_ROOT_DIR="${CMAKE_SOURCE_DIR}"
    YTEST_BUILD_DIR="${CMAKE_BINARY_DIR}"
)
target_link_libraries(yappod_test_support PRIVATE ${BDB_LIB} ZLIB::ZLIB Threads::Threads)
if(M_LIB)
  target_link_libraries(yappod_test_support PRIVATE ${M_LIB})
endif()

if(TARGET cmocka::cmocka)
  set(CMOCKA_TARGET cmocka::cmocka)
elseif(TARGET cmocka)
  set(CMOCKA_TARGET cmocka)
elseif(DEFINED CMOCKA_LIBRARIES)
  set(CMOCKA_TARGET ${CMOCKA_LIBRARIES})
else()
  set(CMOCKA_TARGET cmocka)
endif()

function(add_yappod_cmocka_test test_name test_source)
  cmake_parse_arguments(ARG "RUN_SERIAL" "LABEL" "EXTRA_SOURCES" ${ARGN})
  set(target_name ${test_name}_test)

  add_executable(${target_name}
    ${test_source}
    ${ARG_EXTRA_SOURCES}
  )
  target_include_directories(${target_name}
    PRIVATE ${SRC_DIR} ${TEST_COMMON_DIR} ${BDB_INCLUDE_DIR}
  )
  target_link_libraries(${target_name}
    PRIVATE
      yappod_test_support
      ${CMOCKA_TARGET}
      ${BDB_LIB}
      ZLIB::ZLIB
      Threads::Threads
  )
  if(M_LIB)
    target_link_libraries(${target_name} PRIVATE ${M_LIB})
  endif()

  add_test(NAME ${test_name} COMMAND $<TARGET_FILE:${target_name}>)
  set_tests_properties(${test_name} PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  if(ARG_RUN_SERIAL)
    set_tests_properties(${test_name} PROPERTIES RUN_SERIAL TRUE)
  endif()
  if(ARG_LABEL)
    set_tests_properties(${test_name} PROPERTIES LABELS ${ARG_LABEL})
  endif()
endfunction()

if(YAPPOD_TESTS_DAEMON)
  add_yappod_cmocka_test(
    error_cases
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmocka/suites/error_cases_test.c
    RUN_SERIAL
    LABEL daemon
  )
  add_yappod_cmocka_test(
    front_http_error_cases
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmocka/suites/front_http_error_cases_test.c
    RUN_SERIAL
    LABEL daemon
  )
  add_yappod_cmocka_test(
    daemon_port_option_cases
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmocka/suites/daemon_port_option_cases_test.c
    RUN_SERIAL
    LABEL daemon
  )
endif()

if(YAPPOD_TESTS_NODAEMON)
  add_yappod_cmocka_test(
    e2e
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmocka/suites/e2e_test.c
    LABEL standalone
  )
  add_yappod_cmocka_test(
    makeindex_input_error_cases
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmocka/suites/makeindex_input_error_cases_test.c
    LABEL standalone
  )
  add_yappod_cmocka_test(
    mergepos_error_cases
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmocka/suites/mergepos_error_cases_test.c
    LABEL standalone
  )
  add_yappod_cmocka_test(
    opendir_error_cases
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmocka/suites/opendir_error_cases_test.c
    LABEL standalone
  )
  add_yappod_cmocka_test(
    relative_index_dir_cases
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmocka/suites/relative_index_dir_cases_test.c
    LABEL standalone
  )
  add_yappod_cmocka_test(
    search_deletefile_bounds
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/search_deletefile_bounds_test.c
    LABEL standalone
    EXTRA_SOURCES ${YAPPO_BASE_SOURCES} ${YAPPO_SEARCH_SOURCES}
  )
  add_yappod_cmocka_test(
    filedata_decode_bounds
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/filedata_decode_bounds_test.c
    LABEL standalone
    EXTRA_SOURCES ${YAPPO_BASE_SOURCES} ${YAPPO_SEARCH_SOURCES}
  )
  add_yappod_cmocka_test(
    search_deletefile_concurrency
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/search_deletefile_concurrency_test.c
    LABEL standalone
    EXTRA_SOURCES ${YAPPO_BASE_SOURCES} ${YAPPO_SEARCH_SOURCES}
  )
  add_yappod_cmocka_test(
    index_varint_decode
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/index_varint_decode_test.c
    LABEL standalone
    EXTRA_SOURCES ${SRC_DIR}/yappo_alloc.c ${SRC_DIR}/yappo_index.c
  )
  add_yappod_cmocka_test(
    seek_offset_overflow
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/seek_offset_overflow_test.c
    LABEL standalone
    EXTRA_SOURCES ${SRC_DIR}/yappo_io.c
  )
  add_yappod_cmocka_test(
    pos_index_bounds
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/pos_index_bounds_test.c
    LABEL standalone
    EXTRA_SOURCES ${SRC_DIR}/yappo_alloc.c ${SRC_DIR}/yappo_io.c ${SRC_DIR}/yappo_index_pos.c
  )
endif()
