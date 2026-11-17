cmake_minimum_required(VERSION 3.18)

project(js_mutator LANGUAGES C CXX)

include(FetchContent)

# Populate pinned sources without invoking either upstream build system.
FetchContent_Declare(tree_sitter_runtime
  GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter.git
  GIT_TAG 3fb7be49145acfb07e33b61ed5689e2b6771b2e9 # v0.27.2
  SOURCE_SUBDIR cmake-not-used)
FetchContent_Declare(tree_sitter_javascript
  GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-javascript.git
  GIT_TAG 44c892e0be055ac465d5eeddae6d3e194424e7de # v0.25.0
  SOURCE_SUBDIR cmake-not-used)
FetchContent_MakeAvailable(tree_sitter_runtime tree_sitter_javascript)

add_library(js_mutator_tree_sitter STATIC
  "${tree_sitter_runtime_SOURCE_DIR}/lib/src/lib.c")
target_include_directories(js_mutator_tree_sitter
  PUBLIC "${tree_sitter_runtime_SOURCE_DIR}/lib/include"
  PRIVATE "${tree_sitter_runtime_SOURCE_DIR}/lib/src")
target_compile_features(js_mutator_tree_sitter PRIVATE c_std_11)
set_target_properties(js_mutator_tree_sitter PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(js_mutator_javascript_grammar STATIC
  "${tree_sitter_javascript_SOURCE_DIR}/src/parser.c"
  "${tree_sitter_javascript_SOURCE_DIR}/src/scanner.c")
target_include_directories(js_mutator_javascript_grammar
  PUBLIC "${tree_sitter_javascript_SOURCE_DIR}/bindings/c"
  PRIVATE "${tree_sitter_javascript_SOURCE_DIR}/src")
target_compile_features(js_mutator_javascript_grammar PRIVATE c_std_11)
set_target_properties(js_mutator_javascript_grammar PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(js_mutator STATIC src/mutator.cc)
target_compile_features(js_mutator PUBLIC cxx_std_17)
target_include_directories(js_mutator PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(js_mutator PRIVATE js_mutator_tree_sitter js_mutator_javascript_grammar)
set_target_properties(js_mutator PROPERTIES POSITION_INDEPENDENT_CODE ON)

option(JS_MUTATOR_BUILD_LIBFUZZER_ADAPTER "Build the libFuzzer callback adapter" ON)
if(JS_MUTATOR_BUILD_LIBFUZZER_ADAPTER)
  add_library(js_mutator_libfuzzer OBJECT adapters/libfuzzer.cc)
  target_link_libraries(js_mutator_libfuzzer PUBLIC js_mutator)
endif()

include(CTest)
if(BUILD_TESTING)
  add_executable(js_mutator_test tests/mutator_test.cc)
  target_link_libraries(js_mutator_test PRIVATE js_mutator)
  add_test(NAME js_mutator_test COMMAND js_mutator_test)
  if(JS_MUTATOR_BUILD_LIBFUZZER_ADAPTER)
    add_executable(js_mutator_adapter_test tests/adapter_test.cc)
    target_link_libraries(js_mutator_adapter_test PRIVATE js_mutator_libfuzzer)
    add_test(NAME js_mutator_adapter_test COMMAND js_mutator_adapter_test)
  endif()
endif()
