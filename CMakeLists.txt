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

add_library(js_mutator STATIC src/mutator.cc src/generator.cc)
target_compile_features(js_mutator PUBLIC cxx_std_17)
target_include_directories(js_mutator PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(js_mutator PRIVATE js_mutator_tree_sitter js_mutator_javascript_grammar)
set_target_properties(js_mutator PROPERTIES POSITION_INDEPENDENT_CODE ON)

option(JS_MUTATOR_BUILD_LIBFUZZER_ADAPTER "Build the libFuzzer callback adapter" ON)
if(JS_MUTATOR_BUILD_LIBFUZZER_ADAPTER)
  add_library(js_mutator_libfuzzer OBJECT adapters/libfuzzer.cc)
  target_link_libraries(js_mutator_libfuzzer PUBLIC js_mutator)
  # The same callbacks writing a per-unit yield log (see tools/yield.py).
  add_library(js_mutator_libfuzzer_yield OBJECT adapters/libfuzzer.cc)
  target_link_libraries(js_mutator_libfuzzer_yield PUBLIC js_mutator)
  target_compile_definitions(js_mutator_libfuzzer_yield PRIVATE JS_MUTATOR_YIELD_LOG)
endif()

include(CTest)
if(BUILD_TESTING)
  foreach(test_name mutator generation)
    add_executable(js_mutator_${test_name}_test tests/${test_name}_test.cc)
    target_link_libraries(js_mutator_${test_name}_test PRIVATE js_mutator js_mutator_tree_sitter
                                                              js_mutator_javascript_grammar)
    target_include_directories(js_mutator_${test_name}_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    add_test(NAME js_mutator_${test_name}_test COMMAND js_mutator_${test_name}_test)
  endforeach()
  if(JS_MUTATOR_BUILD_LIBFUZZER_ADAPTER)
    add_executable(js_mutator_adapter_test tests/adapter_test.cc)
    target_link_libraries(js_mutator_adapter_test PRIVATE js_mutator_libfuzzer
                                                         js_mutator_tree_sitter
                                                         js_mutator_javascript_grammar)
    add_test(NAME js_mutator_adapter_test COMMAND js_mutator_adapter_test)
    set_tests_properties(js_mutator_adapter_test PROPERTIES ENVIRONMENT "JS_MUTATOR_VALID_PERCENT=100")
    add_executable(js_mutator_adapter_yield_test tests/adapter_test.cc)
    target_compile_definitions(js_mutator_adapter_yield_test PRIVATE JS_MUTATOR_YIELD_TEST)
    target_link_libraries(js_mutator_adapter_yield_test PRIVATE js_mutator_libfuzzer_yield
                                                               js_mutator_tree_sitter
                                                               js_mutator_javascript_grammar)
  endif()
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_FOUND)
    if(JS_MUTATOR_BUILD_LIBFUZZER_ADAPTER)
      add_test(NAME js_mutator_yield_test
               COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/yield_test.py
                       $<TARGET_FILE:js_mutator_adapter_yield_test>)
      set_tests_properties(js_mutator_yield_test PROPERTIES ENVIRONMENT "JS_MUTATOR_VALID_PERCENT=100")
    endif()
    add_test(NAME js_mutator_cov_summary_test
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/cov_summary_test.py)
  endif()
  # Optional: check acceptance with V8's own parser when Node.js is installed.
  find_program(JS_MUTATOR_NODE node)
  add_executable(js_mutator_sample tools/sample.cc)
  target_link_libraries(js_mutator_sample PRIVATE js_mutator)
  if(JS_MUTATOR_NODE)
    add_test(NAME js_mutator_v8_parse_test
             COMMAND ${CMAKE_COMMAND} -DSAMPLE=$<TARGET_FILE:js_mutator_sample>
                     -DNODE=${JS_MUTATOR_NODE}
                     -DCHECK=${CMAKE_CURRENT_SOURCE_DIR}/tools/v8_parse_check.js
                     -DOUT=${CMAKE_CURRENT_BINARY_DIR}/v8_samples.txt
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/v8_parse_test.cmake)
  endif()
endif()
