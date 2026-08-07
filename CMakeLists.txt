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

find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
  add_test(
    NAME wikipedia_example_converter
    COMMAND ${Python3_EXECUTABLE} -m unittest discover
            -s ${CMAKE_CURRENT_SOURCE_DIR}/examples/wikipedia-search/tests
            -p test_*.py
  )
  add_test(
    NAME systemd_examples
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/acceptance/systemd_examples_test.py
            ${CMAKE_CURRENT_SOURCE_DIR}
  )
  set_tests_properties(systemd_examples PROPERTIES LABELS standalone)
endif()

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

if(Python3_Interpreter_FOUND)
  add_test(
    NAME source_dependency_rules
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/support/check_src_dependencies.py
            ${SRC_DIR}
  )
  set_tests_properties(source_dependency_rules PROPERTIES LABELS standalone)
endif()

include(CheckIncludeFile)
set(CMAKE_REQUIRED_INCLUDES ${SRC_DIR})
foreach(header IN ITEMS common/yappo_types_v2.h config/yappo_config_v2.h storage/yappo_storage_v2.h)
  string(MAKE_C_IDENTIFIER "YAPPOD_HAS_SELF_CONTAINED_${header}" result_variable)
  check_include_file(${header} ${result_variable})
  if(NOT ${result_variable})
    message(FATAL_ERROR "${header} must compile as a self-contained header")
  endif()
endforeach()
unset(CMAKE_REQUIRED_INCLUDES)

set(YAPPOD_COMMON_SOURCES
  ${SRC_DIR}/common/yappo_io.c
  ${SRC_DIR}/common/yappo_net.c
  ${SRC_DIR}/common/yappo_types_v2.c
  ${SRC_DIR}/common/yappo_unicode.c
)

set(YAPPOD_CONFIG_SOURCES
  ${SRC_DIR}/config/yappo_config_v2.c
  ${SRC_DIR}/config/yappo_application_config.c
  ${SRC_DIR}/config/yappo_compaction_policy_v2.c
  ${SRC_DIR}/config/yappo_runtime_policy_v2.c
)

set(YAPPOD_STORAGE_SOURCES
  ${SRC_DIR}/storage/yappo_storage_v2.c
  ${SRC_DIR}/storage/yappo_compaction_status_v2.c
  ${SRC_DIR}/storage/yappo_segment_v2.c
  ${SRC_DIR}/storage/yappo_manifest_v2.c
  ${SRC_DIR}/storage/yappo_snapshot_v2.c
  ${SRC_DIR}/storage/yappo_writer_lock_v2.c
)

set(YAPPOD_COMPONENTS_SOURCES
  ${SRC_DIR}/components/yappo_lexical_v2.c
  ${SRC_DIR}/components/yappo_lexical_reader_v2.c
  ${SRC_DIR}/components/yappo_metadata_v2.c
  ${SRC_DIR}/components/yappo_embedding.c
  ${SRC_DIR}/components/yappo_vector.c
  ${SRC_DIR}/components/yappo_vector_v2.c
  ${SRC_DIR}/components/yappo_ann_v2.c
)

set(YAPPOD_QUERY_SOURCES
  ${SRC_DIR}/query/yappo_bm25.c
  ${SRC_DIR}/query/yappo_ann_corpus_v2.c
  ${SRC_DIR}/query/yappo_filter_v2.c
  ${SRC_DIR}/query/yappo_snippet_v2.c
  ${SRC_DIR}/query/yappo_lexical_search_v2.c
  ${SRC_DIR}/query/yappo_hybrid.c
  ${SRC_DIR}/query/yappo_query_v2.c
  ${SRC_DIR}/query/yappo_retrieve_v2.c
  ${SRC_DIR}/query/yappo_rag.c
)

set(YAPPOD_INDEXING_SOURCES
  ${SRC_DIR}/indexing/yappo_ingest.c
  ${SRC_DIR}/indexing/yappo_build_v2.c
  ${SRC_DIR}/indexing/yappo_update_v2.c
  ${SRC_DIR}/indexing/yappo_segment_planner_v2.c
  ${SRC_DIR}/indexing/yappo_compact_v2.c
)

set(YAPPOD_SERVER_SOURCES
  ${SRC_DIR}/server/yappo_observability_v2.c
  ${SRC_DIR}/server/yappo_core_http_v2.c
  ${SRC_DIR}/server/yappo_executor_v2.c
  ${SRC_DIR}/server/yappo_http_v2.c
)

add_library(yappod_common STATIC ${YAPPOD_COMMON_SOURCES})
add_library(yappod::common ALIAS yappod_common)
target_include_directories(yappod_common PUBLIC ${SRC_DIR})
target_link_libraries(yappod_common PRIVATE ICU::uc ICU::i18n)

add_library(yappod_config STATIC ${YAPPOD_CONFIG_SOURCES})
add_library(yappod::config ALIAS yappod_config)
target_include_directories(yappod_config PUBLIC ${SRC_DIR})
target_link_libraries(yappod_config PRIVATE yappod_common yappod::tomlc99)

add_library(yappod_storage STATIC ${YAPPOD_STORAGE_SOURCES})
add_library(yappod::storage ALIAS yappod_storage)
target_include_directories(yappod_storage PUBLIC ${SRC_DIR})
target_link_libraries(yappod_storage PRIVATE yappod_config Threads::Threads)

add_library(yappod_components STATIC ${YAPPOD_COMPONENTS_SOURCES})
add_library(yappod::components ALIAS yappod_components)
target_include_directories(yappod_components PUBLIC ${SRC_DIR})
target_link_libraries(yappod_components
  PUBLIC yappod::usearch
  PRIVATE yappod_storage yappod::yyjson ICU::uc ICU::i18n CURL::libcurl Threads::Threads)

add_library(yappod_query STATIC ${YAPPOD_QUERY_SOURCES})
add_library(yappod::query ALIAS yappod_query)
target_include_directories(yappod_query PUBLIC ${SRC_DIR})
target_link_libraries(yappod_query
  PUBLIC yappod_components
  PRIVATE yappod::yyjson ICU::uc ICU::i18n)

add_library(yappod_indexing STATIC ${YAPPOD_INDEXING_SOURCES})
add_library(yappod::indexing ALIAS yappod_indexing)
target_include_directories(yappod_indexing PUBLIC ${SRC_DIR})
target_link_libraries(yappod_indexing PRIVATE yappod_components yappod::yyjson)

add_library(yappod_server STATIC ${YAPPOD_SERVER_SOURCES})
add_library(yappod::server ALIAS yappod_server)
target_include_directories(yappod_server PUBLIC ${SRC_DIR})
target_link_libraries(yappod_server PRIVATE yappod_query yappod_indexing
  yappod::yyjson CURL::libcurl Threads::Threads)

set(YAPPOD_INTERNAL_LIBRARIES
  yappod_common
  yappod_config
  yappod_storage
  yappod_components
  yappod_query
  yappod_indexing
  yappod_server
)

set_property(TARGET yappod_config PROPERTY YAPPOD_ALLOWED_INTERNAL_DEPENDENCIES yappod_common)
set_property(TARGET yappod_storage PROPERTY YAPPOD_ALLOWED_INTERNAL_DEPENDENCIES yappod_config)
set_property(TARGET yappod_components PROPERTY YAPPOD_ALLOWED_INTERNAL_DEPENDENCIES yappod_storage)
set_property(TARGET yappod_query PROPERTY YAPPOD_ALLOWED_INTERNAL_DEPENDENCIES yappod_components)
set_property(TARGET yappod_indexing PROPERTY YAPPOD_ALLOWED_INTERNAL_DEPENDENCIES yappod_components)
set_property(TARGET yappod_server PROPERTY YAPPOD_ALLOWED_INTERNAL_DEPENDENCIES
  yappod_query yappod_indexing)

foreach(tgt IN LISTS YAPPOD_INTERNAL_LIBRARIES)
  yappod_enable_warnings(${tgt})
endforeach()

function(yappod_validate_internal_dag)
  foreach(tgt IN LISTS YAPPOD_INTERNAL_LIBRARIES)
    get_target_property(internal_links ${tgt} LINK_LIBRARIES)
    get_target_property(allowed_internal_links ${tgt} YAPPOD_ALLOWED_INTERNAL_DEPENDENCIES)
    foreach(link IN LISTS internal_links)
      if(link IN_LIST YAPPOD_INTERNAL_LIBRARIES AND
         NOT link IN_LIST allowed_internal_links)
        message(FATAL_ERROR "${tgt} has forbidden internal dependency ${link}")
      endif()
    endforeach()
  endforeach()
endfunction()

add_executable(search ${SRC_DIR}/app/search.c)

add_executable(yappo_makeindex ${SRC_DIR}/app/yappo_makeindex.c)

add_executable(yappo_compact ${SRC_DIR}/app/yappo_compact.c)

add_executable(yappod_core ${SRC_DIR}/app/yappod_core.c)

add_executable(yappod_front ${SRC_DIR}/app/yappod_front.c)

set(ALL_TARGETS
  search
  yappo_makeindex
  yappo_compact
  yappod_core
  yappod_front
)

target_link_libraries(search PRIVATE yappod_server yappod::yyjson)
target_link_libraries(yappo_makeindex PRIVATE yappod_server)
target_link_libraries(yappo_compact PRIVATE yappod_indexing)
target_link_libraries(yappod_core PRIVATE yappod_server)
target_link_libraries(yappod_front PRIVATE yappod_server)

function(yappod_assert_executable_internal_link target expected)
  get_target_property(links ${target} LINK_LIBRARIES)
  set(internal_links)
  foreach(link IN LISTS links)
    if(link IN_LIST YAPPOD_INTERNAL_LIBRARIES)
      list(APPEND internal_links ${link})
    endif()
  endforeach()
  if(NOT "${internal_links}" STREQUAL "${expected}")
    message(FATAL_ERROR
      "${target} must link only its top-level internal library ${expected}; got ${internal_links}")
  endif()
endfunction()

foreach(tgt IN LISTS ALL_TARGETS)
  target_include_directories(${tgt} PRIVATE ${SRC_DIR})
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
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/config.example.toml
  DESTINATION share/yappod
)

add_test(
  NAME install_config_smoke
  COMMAND ${CMAKE_COMMAND} -DBUILD_DIR=${CMAKE_BINARY_DIR}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/acceptance/install_config_smoke.cmake
)
set_tests_properties(install_config_smoke PROPERTIES LABELS standalone RUN_SERIAL TRUE)

set(TEST_COMMON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/tests/support)

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
  cmake_parse_arguments(ARG "RUN_SERIAL" "LABEL" "LIBRARIES;EXTRA_SOURCES" ${ARGN})
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
      ${ARG_LIBRARIES}
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
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/server/http_v2_daemon_test.c
    RUN_SERIAL
    LABEL daemon
    LIBRARIES yappod_components
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
  target_link_libraries(v2_daemon_reliability_test PRIVATE yappod_server yappod_components)
  set_tests_properties(v2_daemon_reliability PROPERTIES TIMEOUT 120)
endif()

if(YAPPOD_TESTS_NODAEMON)
  set(QUALITY_TEST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/tests/quality)

  add_executable(dependency_smoke_test
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/acceptance/dependency_smoke_test.c
  )
  yappod_enable_warnings(dependency_smoke_test)
  target_link_libraries(dependency_smoke_test PRIVATE yappod::modern_dependencies)
  set_target_properties(dependency_smoke_test PROPERTIES LINKER_LANGUAGE CXX)
  add_test(NAME dependency_smoke COMMAND $<TARGET_FILE:dependency_smoke_test>)
  set_tests_properties(dependency_smoke PROPERTIES LABELS standalone)

  add_yappod_cmocka_test(
    seek_offset_overflow
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/common/seek_offset_overflow_test.c
    LABEL standalone
    LIBRARIES yappod_common
  )
  add_yappod_cmocka_test(
    v2_cli_acceptance
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/acceptance/v2_cli_acceptance_test.c
    LABEL standalone
  )
  add_yappod_cmocka_test(
    canonical_ingest
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/indexing/canonical_ingest_test.c
    LABEL standalone
    LIBRARIES yappod_indexing
  )
  add_yappod_cmocka_test(
    unicode_tokenizer
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/common/unicode_tokenizer_test.c
    LABEL standalone
    LIBRARIES yappod_common
  )
  add_yappod_cmocka_test(
    config_v2
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/config/config_v2_test.c
    LABEL standalone
    LIBRARIES yappod_config
  )
  add_yappod_cmocka_test(
    application_config
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/config/application_config_test.c
    LABEL standalone
    LIBRARIES yappod_config
  )
  add_yappod_cmocka_test(
    index_v2_contracts
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/storage/index_v2_contracts_test.c
    LABEL standalone
    LIBRARIES yappod_storage
  )
  add_yappod_cmocka_test(
    segment_v2_roundtrip
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/storage/segment_v2_roundtrip_test.c
    LABEL standalone
    LIBRARIES yappod_storage
  )
  add_yappod_cmocka_test(
    rag_passage
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/query/rag_passage_test.c
    LABEL standalone
    LIBRARIES yappod_query
  )
  add_yappod_cmocka_test(
    vector_search
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/components/vector_search_test.c
    LABEL standalone
    LIBRARIES yappod_components
  )
  add_yappod_cmocka_test(
    hybrid_rerank
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/query/hybrid_rerank_test.c
    LABEL standalone
    LIBRARIES yappod_query
  )
  add_yappod_cmocka_test(
    bm25_score
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/query/bm25_score_test.c
    LABEL standalone
    LIBRARIES yappod_query
  )
  add_yappod_cmocka_test(
    manifest_v2_nrt
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/storage/manifest_v2_nrt_test.c
    LABEL standalone
    LIBRARIES yappod_storage
  )
  add_yappod_cmocka_test(
    lexical_v2_writer
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/components/lexical_v2_writer_test.c
    LABEL standalone
    LIBRARIES yappod_components
  )
  add_yappod_cmocka_test(
    lexical_v2_reader
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/components/lexical_v2_reader_test.c
    LABEL standalone
    LIBRARIES yappod_components
  )
  add_yappod_cmocka_test(
    lexical_v2_ranking
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/query/lexical_v2_ranking_test.c
    LABEL standalone
    LIBRARIES yappod_query
  )
  add_yappod_cmocka_test(
    metadata_filter_snippet_v2
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/query/metadata_filter_snippet_v2_test.c
    LABEL standalone
    LIBRARIES yappod_query
  )
  add_yappod_cmocka_test(
    embedding_provider
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/components/embedding_provider_test.c
    LABEL standalone
    LIBRARIES yappod_components
  )
  target_link_libraries(embedding_provider_test PRIVATE yappod::yyjson)
  add_yappod_cmocka_test(
    vector_v2_persistence
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/components/vector_v2_persistence_test.c
    LABEL standalone
    LIBRARIES yappod_components
  )
  add_yappod_cmocka_test(
    ann_v2
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/components/ann_v2_test.c
    LABEL standalone
    LIBRARIES yappod_components
  )
  add_yappod_cmocka_test(
    snapshot_v2
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/storage/snapshot_v2_test.c
    LABEL standalone
    LIBRARIES yappod_storage
  )
  add_yappod_cmocka_test(
    query_v2
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/query/query_v2_test.c
    LABEL standalone
    LIBRARIES yappod_query
  )
  add_yappod_cmocka_test(
    ann_corpus_v2
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/query/ann_corpus_v2_test.c
    LABEL standalone
    LIBRARIES yappod_query
  )
  add_yappod_cmocka_test(
    retrieve_v2
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/query/retrieve_v2_test.c
    LABEL standalone
    LIBRARIES yappod_query
  )
  add_yappod_cmocka_test(
    core_http_v2
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/server/core_http_v2_test.c
    LABEL standalone
    LIBRARIES yappod_server
  )
  add_yappod_cmocka_test(
    executor_v2
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/server/executor_v2_test.c
    LABEL standalone
    LIBRARIES yappod_server
  )
  add_yappod_cmocka_test(
    http_v2_runtime
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/server/http_v2_runtime_test.c
    LABEL standalone
    LIBRARIES yappod_server
  )
  target_link_libraries(http_v2_runtime_test PRIVATE yappod::yyjson)
  add_yappod_cmocka_test(
    update_v2
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/indexing/update_v2_test.c
    LABEL standalone
    LIBRARIES yappod_server
  )
  target_link_libraries(update_v2_test PRIVATE yappod::yyjson)
  add_yappod_cmocka_test(
    segment_planner_v2
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/indexing/segment_planner_v2_test.c
    LABEL standalone
    LIBRARIES yappod_indexing
  )
  add_yappod_cmocka_test(
    compaction_policy_v2
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/indexing/compaction_policy_v2_test.c
    LABEL standalone
    LIBRARIES yappod_indexing
  )
  add_yappod_cmocka_test(
    runtime_policy_v2
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/config/runtime_policy_v2_test.c
    LABEL standalone
    LIBRARIES yappod_config
  )
  add_yappod_cmocka_test(
    observability_v2
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/server/observability_v2_test.c
    LABEL standalone
    LIBRARIES yappod_server
  )
  add_yappod_cmocka_test(
    search_quality_metrics
    ${QUALITY_TEST_DIR}/search_quality_metrics_test.c
    LABEL standalone
    EXTRA_SOURCES ${QUALITY_TEST_DIR}/search_quality_metrics.c
  )
  target_include_directories(search_quality_metrics_test PRIVATE ${QUALITY_TEST_DIR})

  add_yappod_cmocka_test(
    segment_health_regression
    ${QUALITY_TEST_DIR}/segment_health_regression_test.c
    LABEL standalone
    EXTRA_SOURCES ${QUALITY_TEST_DIR}/v2_quality_fixture.c
    LIBRARIES yappod_server
  )
  target_include_directories(segment_health_regression_test PRIVATE
    ${QUALITY_TEST_DIR})
  target_link_libraries(segment_health_regression_test PRIVATE
    yappod_components yappod::yyjson)

  add_executable(v2_load_probe ${QUALITY_TEST_DIR}/v2_load_probe.c)
  yappod_enable_warnings(v2_load_probe)
  target_include_directories(v2_load_probe PRIVATE ${TEST_COMMON_DIR})
  target_link_libraries(v2_load_probe PRIVATE yappod_test_support yappod::yyjson Threads::Threads)

  add_executable(v2_segment_search_benchmark
    ${QUALITY_TEST_DIR}/v2_segment_search_benchmark.c
    ${QUALITY_TEST_DIR}/search_quality_metrics.c
  )
  yappod_enable_warnings(v2_segment_search_benchmark)
  target_include_directories(v2_segment_search_benchmark PRIVATE
    ${TEST_COMMON_DIR} ${QUALITY_TEST_DIR})
  target_link_libraries(v2_segment_search_benchmark PRIVATE
    yappod_server yappod_components yappod_test_support)

  add_executable(v2_ann_segment_benchmark
    ${QUALITY_TEST_DIR}/v2_ann_segment_benchmark.c
  )
  yappod_enable_warnings(v2_ann_segment_benchmark)
  target_link_libraries(v2_ann_segment_benchmark PRIVATE
    yappod_components m)

  add_yappod_cmocka_test(
    v2_search_quality
    ${QUALITY_TEST_DIR}/v2_search_quality_test.c
    LABEL standalone
    EXTRA_SOURCES ${QUALITY_TEST_DIR}/v2_quality_fixture.c
                  ${QUALITY_TEST_DIR}/search_quality_metrics.c
  )
  target_include_directories(v2_search_quality_test PRIVATE ${QUALITY_TEST_DIR})
  target_link_libraries(v2_search_quality_test PRIVATE
    yappod_server yappod_indexing yappod_components yappod::yyjson)
endif()

if(YAPPOD_BUILD_FUZZERS)
  if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "YAPPOD_BUILD_FUZZERS requires Clang")
  endif()

  add_executable(core_http_v2_fuzz
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/core_http_v2_fuzz.c
  )
  target_include_directories(core_http_v2_fuzz PRIVATE ${SRC_DIR})
  target_link_libraries(core_http_v2_fuzz PRIVATE yappod_server)
  target_compile_options(core_http_v2_fuzz PRIVATE -fsanitize=fuzzer)
  target_link_options(core_http_v2_fuzz PRIVATE -fsanitize=fuzzer)
  yappod_enable_warnings(core_http_v2_fuzz)

  add_executable(ingest_v2_fuzz
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/ingest_v2_fuzz.c
  )
  target_include_directories(ingest_v2_fuzz PRIVATE ${SRC_DIR})
  target_compile_options(ingest_v2_fuzz PRIVATE -fsanitize=fuzzer)
  target_link_options(ingest_v2_fuzz PRIVATE -fsanitize=fuzzer)
  target_link_libraries(ingest_v2_fuzz PRIVATE yappod_indexing)
  yappod_enable_warnings(ingest_v2_fuzz)
endif()

yappod_validate_internal_dag()
yappod_assert_executable_internal_link(search yappod_server)
yappod_assert_executable_internal_link(yappo_makeindex yappod_server)
yappod_assert_executable_internal_link(yappo_compact yappod_indexing)
yappod_assert_executable_internal_link(yappod_core yappod_server)
yappod_assert_executable_internal_link(yappod_front yappod_server)

if(TARGET yappod_base)
  message(FATAL_ERROR "The retired yappod_base target must not be reintroduced")
endif()
