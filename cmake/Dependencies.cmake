include(FetchContent)

if(APPLE)
  find_program(YAPPOD_HOMEBREW_EXECUTABLE brew
    HINTS /opt/homebrew/bin /usr/local/bin
  )
  if(YAPPOD_HOMEBREW_EXECUTABLE)
    set(YAPPOD_HOMEBREW_PREFIXES "")
    foreach(formula IN ITEMS icu4c libevent curl)
      execute_process(
        COMMAND "${YAPPOD_HOMEBREW_EXECUTABLE}" --prefix "${formula}"
        RESULT_VARIABLE formula_result
        OUTPUT_VARIABLE formula_prefix
        ERROR_QUIET
        OUTPUT_STRIP_TRAILING_WHITESPACE
      )
      if(formula_result EQUAL 0 AND IS_DIRECTORY "${formula_prefix}")
        list(APPEND YAPPOD_HOMEBREW_PREFIXES "${formula_prefix}")
      endif()
    endforeach()
    list(APPEND CMAKE_PREFIX_PATH ${YAPPOD_HOMEBREW_PREFIXES})
    if(YAPPOD_HOMEBREW_PREFIXES)
      message(STATUS "Homebrew dependency prefixes: ${YAPPOD_HOMEBREW_PREFIXES}")
    endif()
  endif()
endif()

find_package(ICU 67 REQUIRED COMPONENTS uc i18n)
find_package(CURL 7.68 REQUIRED)
find_package(Libevent REQUIRED)

set(FETCHCONTENT_UPDATES_DISCONNECTED ON)

FetchContent_Declare(
  yyjson
  GIT_REPOSITORY https://github.com/ibireme/yyjson.git
  GIT_TAG 8b4a38dc994a110abaec8a400615567bd996105f
  GIT_SUBMODULES ""
  SOURCE_SUBDIR _yappod_no_cmake
)
FetchContent_Declare(
  tomlc99
  GIT_REPOSITORY https://github.com/cktan/tomlc99.git
  GIT_TAG 29076dfd095bbbbd50a3c1b2760d29f4b83e74ac
  GIT_SUBMODULES ""
  SOURCE_SUBDIR _yappod_no_cmake
)
FetchContent_Declare(
  usearch
  GIT_REPOSITORY https://github.com/unum-cloud/usearch.git
  GIT_TAG 40d127f472e9073875566f0e9308c0302b89100a
  GIT_SUBMODULES ""
  SOURCE_SUBDIR _yappod_no_cmake
)

FetchContent_MakeAvailable(yyjson tomlc99 usearch)

add_library(yappod_yyjson STATIC
  ${yyjson_SOURCE_DIR}/src/yyjson.c
)
add_library(yappod::yyjson ALIAS yappod_yyjson)
target_include_directories(yappod_yyjson SYSTEM PUBLIC
  ${yyjson_SOURCE_DIR}/src
)
set_target_properties(yappod_yyjson PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(yappod_tomlc99 STATIC
  ${tomlc99_SOURCE_DIR}/toml.c
)
add_library(yappod::tomlc99 ALIAS yappod_tomlc99)
target_include_directories(yappod_tomlc99 SYSTEM PUBLIC
  ${tomlc99_SOURCE_DIR}
)
set_target_properties(yappod_tomlc99 PROPERTIES POSITION_INDEPENDENT_CODE ON)

# USearch is a C++ implementation with a stable C99 facade. Keep C++ isolated in
# this target so the Yappod2 implementation remains C99.
add_library(yappod_usearch STATIC
  ${usearch_SOURCE_DIR}/c/lib.cpp
)
add_library(yappod::usearch ALIAS yappod_usearch)
target_include_directories(yappod_usearch SYSTEM PUBLIC
  ${usearch_SOURCE_DIR}/c
  ${usearch_SOURCE_DIR}/include
)
target_compile_features(yappod_usearch PRIVATE cxx_std_11)
target_compile_definitions(yappod_usearch PRIVATE
  USEARCH_USE_FP16LIB=0
  USEARCH_USE_OPENMP=0
  USEARCH_USE_SIMSIMD=0
)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(yappod_usearch PRIVATE -Wno-cpp)
endif()
set_target_properties(yappod_usearch PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(yappod_modern_dependencies INTERFACE)
add_library(yappod::modern_dependencies ALIAS yappod_modern_dependencies)
target_link_libraries(yappod_modern_dependencies INTERFACE
  ICU::uc
  ICU::i18n
  CURL::libcurl
  Libevent::core
  yappod::yyjson
  yappod::tomlc99
  yappod::usearch
)
