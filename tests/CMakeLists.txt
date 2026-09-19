# Microburst Governor test suite.
# Copyright 2026 Summon Software Labs.
# SPDX-License-Identifier: Apache-2.0
#
# No test carries a timeout property: a hanging test is a defect that must be diagnosed, not
# terminated by a watchdog.

function(mbg_add_test target)
  add_executable(${target} ${ARGN})
  target_link_libraries(${target} PRIVATE mbg mbg_warnings)
  target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  if(MSVC)
    target_compile_options(${target} PRIVATE /bigobj)
  endif()
  add_test(NAME ${target} COMMAND ${target})
  set_tests_properties(${target} PROPERTIES LABELS "mbg")
endfunction()

set(MBG_TEST_SUPPORT test_main.cpp test_framework.cpp)

mbg_add_test(mbg_core_test ${MBG_TEST_SUPPORT} test_core.cpp test_policy_window.cpp)
mbg_add_test(mbg_detector_test ${MBG_TEST_SUPPORT} test_detector.cpp)
mbg_add_test(mbg_governor_test ${MBG_TEST_SUPPORT} test_governor.cpp test_property.cpp
             test_concurrency.cpp test_hardening.cpp)
mbg_add_test(mbg_durability_test ${MBG_TEST_SUPPORT} test_persistence.cpp test_adversarial.cpp)
mbg_add_test(mbg_transport_test ${MBG_TEST_SUPPORT} test_transport.cpp)

# The multiprocess suite provides its own entry point because it also acts as the coordinator and
# worker process image for the crash scenarios it drives.
add_executable(mbg_multiprocess_test test_multiprocess.cpp test_framework.cpp)
target_link_libraries(mbg_multiprocess_test PRIVATE mbg mbg_warnings)
target_include_directories(mbg_multiprocess_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(mbg_multiprocess_test PRIVATE
  MBG_TOOL_PATH="$<TARGET_FILE:mbgctl>")
add_test(NAME mbg_multiprocess_test COMMAND mbg_multiprocess_test)
set_tests_properties(mbg_multiprocess_test PROPERTIES LABELS "mbg")
