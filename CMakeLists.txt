cmake_minimum_required(VERSION 3.20)
project(yappod LANGUAGES C CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()

option(YAPPOD_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(YAPPOD_TESTS_DAEMON "Enable daemon-based test suites" ON)
option(YAPPOD_TESTS_NODAEMON "Enable non-daemon test suites" ON)

function(yappod_enable_warnings target)
  if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    if(YAPPOD_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(${target} PRIVATE _DEFAULT_SOURCE _POSIX_C_SOURCE=200809L)
  endif()
endfunction()

find_package(Threads REQUIRED)
find_package(ZLIB REQUIRED)
find_package(cmocka REQUIRED)
include(Dependencies)

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
  ${SRC_DIR}/yappo_index_v2.c
  ${SRC_DIR}/yappo_config_v2.c
  ${SRC_DIR}/yappo_unicode.c
  ${SRC_DIR}/yappo_ingest.c
  ${SRC_DIR}/yappo_segment_v2.c
  ${SRC_DIR}/yappo_manifest_v2.c
  ${SRC_DIR}/yappo_lexical_v2.c
  ${SRC_DIR}/yappo_search_api.c
  ${SRC_DIR}/yappo_rag.c
  ${SRC_DIR}/yappo_vector.c
  ${SRC_DIR}/yappo_hybrid.c
)

set(YAPPO_SEARCH_SOURCES
  ${SRC_DIR}/yappo_bm25.c
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
  target_link_libraries(${tgt} PRIVATE ${BDB_LIB} ZLIB::ZLIB Threads::Threads yappod::tomlc99 yappod::yyjson ICU::uc ICU::i18n)
  yappod_enable_warnings(${tgt})
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
yappod_enable_warnings(yappod_test_support)
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
  yappod_enable_warnings(${target_name})
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
      yappod::tomlc99
      ICU::uc
      ICU::i18n
      yappod::yyjson
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
  set(QUALITY_TEST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/tests/quality)

  add_executable(dependency_smoke_test
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/dependency_smoke_test.c
  )
  yappod_enable_warnings(dependency_smoke_test)
  target_link_libraries(dependency_smoke_test PRIVATE yappod::modern_dependencies)
  set_target_properties(dependency_smoke_test PROPERTIES LINKER_LANGUAGE CXX)
  add_test(NAME dependency_smoke COMMAND $<TARGET_FILE:dependency_smoke_test>)
  set_tests_properties(dependency_smoke PROPERTIES LABELS standalone)

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
  add_yappod_cmocka_test(
    canonical_ingest
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/canonical_ingest_test.c
    LABEL standalone
    EXTRA_SOURCES ${SRC_DIR}/yappo_index_v2.c ${SRC_DIR}/yappo_config_v2.c ${SRC_DIR}/yappo_unicode.c ${SRC_DIR}/yappo_ingest.c
  )
  add_yappod_cmocka_test(
    unicode_tokenizer
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/unicode_tokenizer_test.c
    LABEL standalone
    EXTRA_SOURCES ${SRC_DIR}/yappo_index_v2.c ${SRC_DIR}/yappo_unicode.c
  )
  add_yappod_cmocka_test(
    config_v2
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/config_v2_test.c
    LABEL standalone
    EXTRA_SOURCES ${SRC_DIR}/yappo_index_v2.c ${SRC_DIR}/yappo_config_v2.c
  )
  target_link_libraries(config_v2_test PRIVATE yappod::tomlc99)
  add_yappod_cmocka_test(
    index_v2_contracts
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/index_v2_contracts_test.c
    LABEL standalone
    EXTRA_SOURCES ${SRC_DIR}/yappo_index_v2.c
  )
  add_yappod_cmocka_test(
    segment_v2_roundtrip
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/segment_v2_roundtrip_test.c
    LABEL standalone
    EXTRA_SOURCES ${SRC_DIR}/yappo_index_v2.c ${SRC_DIR}/yappo_segment_v2.c
  )
  add_yappod_cmocka_test(
    rag_passage
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/rag_passage_test.c
    LABEL standalone
    EXTRA_SOURCES ${SRC_DIR}/yappo_index_v2.c ${SRC_DIR}/yappo_segment_v2.c ${SRC_DIR}/yappo_rag.c
  )
  add_yappod_cmocka_test(
    vector_search
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/vector_search_test.c
    LABEL standalone
    EXTRA_SOURCES ${SRC_DIR}/yappo_index_v2.c ${SRC_DIR}/yappo_vector.c
  )
  add_yappod_cmocka_test(
    hybrid_rerank
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/hybrid_rerank_test.c
    LABEL standalone
    EXTRA_SOURCES ${SRC_DIR}/yappo_index_v2.c ${SRC_DIR}/yappo_hybrid.c
  )
  add_yappod_cmocka_test(
    bm25_score
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/bm25_score_test.c
    LABEL standalone
    EXTRA_SOURCES ${SRC_DIR}/yappo_bm25.c
  )
  add_yappod_cmocka_test(
    manifest_v2_nrt
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/manifest_v2_nrt_test.c
    LABEL standalone
    EXTRA_SOURCES ${SRC_DIR}/yappo_index_v2.c ${SRC_DIR}/yappo_config_v2.c ${SRC_DIR}/yappo_segment_v2.c ${SRC_DIR}/yappo_manifest_v2.c
  )
  add_yappod_cmocka_test(
    lexical_v2_writer
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/lexical_v2_writer_test.c
    LABEL standalone
    EXTRA_SOURCES ${SRC_DIR}/yappo_index_v2.c ${SRC_DIR}/yappo_segment_v2.c ${SRC_DIR}/yappo_unicode.c ${SRC_DIR}/yappo_lexical_v2.c
  )
  add_yappod_cmocka_test(
    search_api
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/search_api_test.c
    LABEL standalone
    EXTRA_SOURCES ${SRC_DIR}/yappo_search_api.c
  )

  add_yappod_cmocka_test(
    search_quality_metrics
    ${QUALITY_TEST_DIR}/search_quality_metrics_test.c
    LABEL standalone
    EXTRA_SOURCES ${QUALITY_TEST_DIR}/search_quality_metrics.c
  )
  target_include_directories(search_quality_metrics_test PRIVATE ${QUALITY_TEST_DIR})

  add_executable(search_quality
    ${QUALITY_TEST_DIR}/search_quality.c
    ${QUALITY_TEST_DIR}/search_quality_metrics.c
  )
  yappod_enable_warnings(search_quality)
  target_include_directories(search_quality
    PRIVATE ${QUALITY_TEST_DIR} ${TEST_COMMON_DIR}
  )
  target_link_libraries(search_quality PRIVATE yappod_test_support)
  if(M_LIB)
    target_link_libraries(search_quality PRIVATE ${M_LIB})
  endif()

  add_test(
    NAME search_quality_baseline
    COMMAND $<TARGET_FILE:search_quality>
      --build-dir ${CMAKE_BINARY_DIR}
      --input ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/index.txt
      --queries ${QUALITY_TEST_DIR}/queries.tsv
      --qrels ${QUALITY_TEST_DIR}/qrels.tsv
      --baseline ${QUALITY_TEST_DIR}/baseline.tsv
      --repeat 5
  )
  set_tests_properties(search_quality_baseline PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    LABELS standalone
  )
endif()
