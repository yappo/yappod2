if(NOT DEFINED BUILD_DIR)
  message(FATAL_ERROR "BUILD_DIR is required")
endif()

set(work "${BUILD_DIR}/install-config-smoke")
set(prefix "${work}/prefix")
set(index "${work}/index")
set(config "${work}/config.toml")
set(input "${work}/documents.ndjson")
file(REMOVE_RECURSE "${work}")
file(MAKE_DIRECTORY "${work}")

execute_process(
  COMMAND "${CMAKE_COMMAND}" --install "${BUILD_DIR}" --prefix "${prefix}"
  RESULT_VARIABLE result
)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "cmake install failed")
endif()

file(WRITE "${config}" "schema_version=1\nformat_version=2\nindex.directory='${index}'\n[tokenizer]\nid='unicode_nfkc_casefold_v2'\n[chunking]\nmax_chars=1200\noverlap_chars=200\n[vector]\nenabled=false\n[metadata]\nfilterable_fields=[]\n[daemon]\nrun_directory='${work}/run'\ncore_host='127.0.0.1'\ncore_port=18401\nfront_host='127.0.0.1'\nfront_port=18400\n")
file(WRITE "${input}" "{\"operation\":\"upsert\",\"id\":\"installed\",\"url\":\"https://example.test/installed\",\"title\":\"Installed\",\"body\":\"installed config smoke\",\"metadata\":{}}\n")

execute_process(
  COMMAND "${prefix}/bin/yappo_makeindex" build --config "${config}" --input "${input}"
  RESULT_VARIABLE result
)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "installed yappo_makeindex build failed")
endif()
execute_process(
  COMMAND "${prefix}/bin/search" --config "${config}" --mode lexical --query smoke
  RESULT_VARIABLE result OUTPUT_VARIABLE output
)
if(NOT result EQUAL 0 OR NOT output MATCHES "installed")
  message(FATAL_ERROR "installed search failed")
endif()
execute_process(
  COMMAND "${prefix}/bin/yappo_makeindex" verify --config "${config}"
  RESULT_VARIABLE result
)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "installed verify failed")
endif()

file(REMOVE_RECURSE "${work}")
