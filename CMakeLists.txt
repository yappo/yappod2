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
option(YAPPOD_BUILD_FUZZERS "Build Clang libFuzzer parser targets" OFF)

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
find_package(cmocka REQUIRED)
include(Dependencies)

set(SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)

set(YAPPO_BASE_SOURCES
  ${SRC_DIR}/yappo_io.c
  ${SRC_DIR}/yappo_net.c
  ${SRC_DIR}/yappo_index_v2.c
  ${SRC_DIR}/yappo_config_v2.c
  ${SRC_DIR}/yappo_unicode.c
  ${SRC_DIR}/yappo_ingest.c
  ${SRC_DIR}/yappo_build_v2.c
  ${SRC_DIR}/yappo_update_v2.c
  ${SRC_DIR}/yappo_writer_lock_v2.c
  ${SRC_DIR}/yappo_runtime_policy_v2.c
  ${SRC_DIR}/yappo_observability_v2.c
  ${SRC_DIR}/yappo_segment_v2.c
  ${SRC_DIR}/yappo_manifest_v2.c
  ${SRC_DIR}/yappo_lexical_v2.c
  ${SRC_DIR}/yappo_lexical_reader_v2.c
  ${SRC_DIR}/yappo_lexical_search_v2.c
  ${SRC_DIR}/yappo_metadata_v2.c
  ${SRC_DIR}/yappo_filter_v2.c
  ${SRC_DIR}/yappo_snippet_v2.c
  ${SRC_DIR}/yappo_embedding.c
  ${SRC_DIR}/yappo_vector_v2.c
  ${SRC_DIR}/yappo_ann_v2.c
  ${SRC_DIR}/yappo_snapshot_v2.c
  ${SRC_DIR}/yappo_query_v2.c
  ${SRC_DIR}/yappo_retrieve_v2.c
  ${SRC_DIR}/yappo_core_protocol_v2.c
  ${SRC_DIR}/yappo_http_v2.c
  ${SRC_DIR}/yappo_bm25.c
  ${SRC_DIR}/yappo_rag.c
  ${SRC_DIR}/yappo_vector.c
  ${SRC_DIR}/yappo_hybrid.c
)

add_library(yappod_base STATIC ${YAPPO_BASE_SOURCES})
target_include_directories(yappod_base PRIVATE ${SRC_DIR})
target_link_libraries(yappod_base PRIVATE Threads::Threads
  yappod::tomlc99 yappod::yyjson yappod::usearch ICU::uc ICU::i18n CURL::libcurl)
yappod_enable_warnings(yappod_base)

add_executable(search ${SRC_DIR}/search.c)

add_executable(yappo_makeindex ${SRC_DIR}/yappo_makeindex.c)

add_executable(yappo_compact
  ${SRC_DIR}/yappo_compact_v2.c
  ${SRC_DIR}/yappo_compact.c
)

add_executable(yappod_core ${SRC_DIR}/yappod_core.c)

add_executable(yappod_front ${SRC_DIR}/yappod_front.c)

set(ALL_TARGETS
  search
  yappo_makeindex
  yappo_compact
  yappod_core
  yappod_front
)

foreach(tgt IN LISTS ALL_TARGETS)
  target_include_directories(${tgt} PRIVATE ${SRC_DIR})
  target_link_libraries(${tgt} PRIVATE yappod_base Threads::Threads yappod::tomlc99 yappod::yyjson yappod::usearch ICU::uc ICU::i18n CURL::libcurl)
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
  ${TEST_COMMON_DIR}/test_proc.c
)

add_library(yappod_test_support STATIC ${TEST_SUPPORT_SOURCES})
yappod_enable_warnings(yappod_test_support)
target_include_directories(yappod_test_support
  PUBLIC ${SRC_DIR} ${TEST_COMMON_DIR}
)
target_compile_definitions(yappod_test_support
  PRIVATE
    YTEST_ROOT_DIR="${CMAKE_SOURCE_DIR}"
    YTEST_BUILD_DIR="${CMAKE_BINARY_DIR}"
)
target_link_libraries(yappod_test_support PRIVATE Threads::Threads)
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
    PRIVATE ${SRC_DIR} ${TEST_COMMON_DIR}
  )
  target_link_libraries(${target_name}
    PRIVATE
      yappod_test_support
      ${CMOCKA_TARGET}
      Threads::Threads
      yappod::tomlc99
      ICU::uc
      ICU::i18n
      yappod::yyjson
      yappod::usearch
      CURL::libcurl
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
    http_v2_daemon
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmocka/suites/http_v2_daemon_test.c
    RUN_SERIAL
    LABEL daemon
    EXTRA_SOURCES ${SRC_DIR}/yappo_index_v2.c ${SRC_DIR}/yappo_config_v2.c ${SRC_DIR}/yappo_segment_v2.c ${SRC_DIR}/yappo_manifest_v2.c ${SRC_DIR}/yappo_unicode.c ${SRC_DIR}/yappo_lexical_v2.c ${SRC_DIR}/yappo_metadata_v2.c ${SRC_DIR}/yappo_vector.c ${SRC_DIR}/yappo_vector_v2.c
  )
  add_yappod_cmocka_test(
    v2_daemon_reliability
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/quality/v2_daemon_reliability_test.c
    RUN_SERIAL
    LABEL daemon
    EXTRA_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/tests/quality/v2_quality_fixture.c
  )
  target_include_directories(v2_daemon_reliability_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/quality)
  target_link_libraries(v2_daemon_reliability_test PRIVATE yappod_base)
  set_tests_properties(v2_daemon_reliability PROPERTIES TIMEOUT 120)
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
    seek_offset_overflow
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/seek_offset_overflow_test.c
    LABEL standalone
    EXTRA_SOURCES ${SRC_DIR}/yappo_io.c
  )
  add_yappod_cmocka_test(
    v2_cli_acceptance
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/v2_cli_acceptance_test.c
    LABEL standalone
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
    lexical_v2_reader
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/lexical_v2_reader_test.c
    LABEL standalone
    EXTRA_SOURCES ${SRC_DIR}/yappo_index_v2.c ${SRC_DIR}/yappo_segment_v2.c ${SRC_DIR}/yappo_unicode.c ${SRC_DIR}/yappo_lexical_v2.c ${SRC_DIR}/yappo_lexical_reader_v2.c
  )
  add_yappod_cmocka_test(
    lexical_v2_ranking
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/lexical_v2_ranking_test.c
    LABEL standalone
    EXTRA_SOURCES ${SRC_DIR}/yappo_index_v2.c ${SRC_DIR}/yappo_segment_v2.c ${SRC_DIR}/yappo_unicode.c ${SRC_DIR}/yappo_lexical_v2.c ${SRC_DIR}/yappo_lexical_reader_v2.c ${SRC_DIR}/yappo_lexical_search_v2.c ${SRC_DIR}/yappo_bm25.c
  )
  add_yappod_cmocka_test(
    metadata_filter_snippet_v2
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/metadata_filter_snippet_v2_test.c
    LABEL standalone
    EXTRA_SOURCES ${SRC_DIR}/yappo_index_v2.c ${SRC_DIR}/yappo_config_v2.c ${SRC_DIR}/yappo_segment_v2.c ${SRC_DIR}/yappo_metadata_v2.c ${SRC_DIR}/yappo_filter_v2.c ${SRC_DIR}/yappo_snippet_v2.c
  )
  target_link_libraries(metadata_filter_snippet_v2_test PRIVATE yappod::tomlc99 yappod::yyjson ICU::uc ICU::i18n)
  add_yappod_cmocka_test(
    embedding_provider
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/embedding_provider_test.c
    LABEL standalone
    EXTRA_SOURCES ${SRC_DIR}/yappo_embedding.c
  )
  target_link_libraries(embedding_provider_test PRIVATE CURL::libcurl yappod::yyjson)
  add_yappod_cmocka_test(
    vector_v2_persistence
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/vector_v2_persistence_test.c
    LABEL standalone
    EXTRA_SOURCES ${SRC_DIR}/yappo_index_v2.c ${SRC_DIR}/yappo_config_v2.c ${SRC_DIR}/yappo_segment_v2.c ${SRC_DIR}/yappo_vector.c ${SRC_DIR}/yappo_vector_v2.c
  )
  add_yappod_cmocka_test(
    ann_v2
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/ann_v2_test.c
    LABEL standalone
    EXTRA_SOURCES ${SRC_DIR}/yappo_index_v2.c ${SRC_DIR}/yappo_config_v2.c ${SRC_DIR}/yappo_segment_v2.c ${SRC_DIR}/yappo_vector.c ${SRC_DIR}/yappo_vector_v2.c ${SRC_DIR}/yappo_ann_v2.c
  )
  add_yappod_cmocka_test(
    snapshot_v2
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/snapshot_v2_test.c
    LABEL standalone
    EXTRA_SOURCES ${SRC_DIR}/yappo_index_v2.c ${SRC_DIR}/yappo_config_v2.c ${SRC_DIR}/yappo_segment_v2.c ${SRC_DIR}/yappo_manifest_v2.c ${SRC_DIR}/yappo_snapshot_v2.c
  )
  add_yappod_cmocka_test(
    query_v2
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/query_v2_test.c
    LABEL standalone
    EXTRA_SOURCES ${SRC_DIR}/yappo_index_v2.c ${SRC_DIR}/yappo_config_v2.c ${SRC_DIR}/yappo_segment_v2.c ${SRC_DIR}/yappo_manifest_v2.c ${SRC_DIR}/yappo_snapshot_v2.c ${SRC_DIR}/yappo_unicode.c ${SRC_DIR}/yappo_lexical_v2.c ${SRC_DIR}/yappo_lexical_reader_v2.c ${SRC_DIR}/yappo_lexical_search_v2.c ${SRC_DIR}/yappo_bm25.c ${SRC_DIR}/yappo_metadata_v2.c ${SRC_DIR}/yappo_filter_v2.c ${SRC_DIR}/yappo_embedding.c ${SRC_DIR}/yappo_vector.c ${SRC_DIR}/yappo_vector_v2.c ${SRC_DIR}/yappo_ann_v2.c ${SRC_DIR}/yappo_hybrid.c ${SRC_DIR}/yappo_query_v2.c
  )
  add_yappod_cmocka_test(
    retrieve_v2
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/retrieve_v2_test.c
    LABEL standalone
    EXTRA_SOURCES ${SRC_DIR}/yappo_index_v2.c ${SRC_DIR}/yappo_config_v2.c ${SRC_DIR}/yappo_segment_v2.c ${SRC_DIR}/yappo_manifest_v2.c ${SRC_DIR}/yappo_snapshot_v2.c ${SRC_DIR}/yappo_retrieve_v2.c
  )
  add_yappod_cmocka_test(
    core_protocol_v2
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/core_protocol_v2_test.c
    LABEL standalone
    EXTRA_SOURCES ${SRC_DIR}/yappo_io.c ${SRC_DIR}/yappo_core_protocol_v2.c
  )
  add_yappod_cmocka_test(
    http_v2_runtime
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/http_v2_runtime_test.c
    LABEL standalone
    EXTRA_SOURCES ${SRC_DIR}/yappo_index_v2.c ${SRC_DIR}/yappo_config_v2.c ${SRC_DIR}/yappo_segment_v2.c ${SRC_DIR}/yappo_manifest_v2.c ${SRC_DIR}/yappo_snapshot_v2.c ${SRC_DIR}/yappo_unicode.c ${SRC_DIR}/yappo_ingest.c ${SRC_DIR}/yappo_update_v2.c ${SRC_DIR}/yappo_writer_lock_v2.c ${SRC_DIR}/yappo_lexical_v2.c ${SRC_DIR}/yappo_lexical_reader_v2.c ${SRC_DIR}/yappo_lexical_search_v2.c ${SRC_DIR}/yappo_bm25.c ${SRC_DIR}/yappo_metadata_v2.c ${SRC_DIR}/yappo_filter_v2.c ${SRC_DIR}/yappo_embedding.c ${SRC_DIR}/yappo_vector.c ${SRC_DIR}/yappo_vector_v2.c ${SRC_DIR}/yappo_ann_v2.c ${SRC_DIR}/yappo_hybrid.c ${SRC_DIR}/yappo_query_v2.c ${SRC_DIR}/yappo_retrieve_v2.c ${SRC_DIR}/yappo_http_v2.c
  )
  add_yappod_cmocka_test(
    update_v2
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/update_v2_test.c
    LABEL standalone
    EXTRA_SOURCES ${SRC_DIR}/yappo_compact_v2.c
  )
  target_link_libraries(update_v2_test PRIVATE yappod_base)
  add_yappod_cmocka_test(
    runtime_policy_v2
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/runtime_policy_v2_test.c
    LABEL standalone
    EXTRA_SOURCES ${SRC_DIR}/yappo_runtime_policy_v2.c
  )
  add_yappod_cmocka_test(
    observability_v2
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/observability_v2_test.c
    LABEL standalone
  )
  target_link_libraries(observability_v2_test PRIVATE yappod_base)
  add_yappod_cmocka_test(
    search_quality_metrics
    ${QUALITY_TEST_DIR}/search_quality_metrics_test.c
    LABEL standalone
    EXTRA_SOURCES ${QUALITY_TEST_DIR}/search_quality_metrics.c
  )
  target_include_directories(search_quality_metrics_test PRIVATE ${QUALITY_TEST_DIR})

  add_executable(v2_load_probe ${QUALITY_TEST_DIR}/v2_load_probe.c)
  yappod_enable_warnings(v2_load_probe)
  target_include_directories(v2_load_probe PRIVATE ${TEST_COMMON_DIR})
  target_link_libraries(v2_load_probe PRIVATE yappod_test_support yappod::yyjson Threads::Threads)

  add_yappod_cmocka_test(
    v2_search_quality
    ${QUALITY_TEST_DIR}/v2_search_quality_test.c
    LABEL standalone
    EXTRA_SOURCES ${QUALITY_TEST_DIR}/v2_quality_fixture.c
                  ${QUALITY_TEST_DIR}/search_quality_metrics.c
  )
  target_include_directories(v2_search_quality_test PRIVATE ${QUALITY_TEST_DIR})
  target_link_libraries(v2_search_quality_test PRIVATE yappod_base)
endif()

if(YAPPOD_BUILD_FUZZERS)
  if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "YAPPOD_BUILD_FUZZERS requires Clang")
  endif()

  add_executable(core_protocol_v2_fuzz
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/core_protocol_v2_fuzz.c
    ${SRC_DIR}/yappo_io.c
    ${SRC_DIR}/yappo_core_protocol_v2.c
  )
  target_include_directories(core_protocol_v2_fuzz PRIVATE ${SRC_DIR})
  target_compile_options(core_protocol_v2_fuzz PRIVATE -fsanitize=fuzzer)
  target_link_options(core_protocol_v2_fuzz PRIVATE -fsanitize=fuzzer)
  yappod_enable_warnings(core_protocol_v2_fuzz)

  add_executable(ingest_v2_fuzz
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/ingest_v2_fuzz.c
    ${SRC_DIR}/yappo_index_v2.c
    ${SRC_DIR}/yappo_config_v2.c
    ${SRC_DIR}/yappo_unicode.c
    ${SRC_DIR}/yappo_ingest.c
  )
  target_include_directories(ingest_v2_fuzz PRIVATE ${SRC_DIR})
  target_compile_options(ingest_v2_fuzz PRIVATE -fsanitize=fuzzer)
  target_link_options(ingest_v2_fuzz PRIVATE -fsanitize=fuzzer)
  target_link_libraries(ingest_v2_fuzz PRIVATE yappod::tomlc99 yappod::yyjson ICU::uc ICU::i18n)
  yappod_enable_warnings(ingest_v2_fuzz)
endif()
