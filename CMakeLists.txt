cmake_minimum_required(VERSION 3.25)

project(mbg
  VERSION 1.0.0
  DESCRIPTION "Microburst Governor: generation-bound detection, classification and bounded response to transient network queue explosions"
  HOMEPAGE_URL "https://github.com/summonlabs/Microburst-Governor"
  LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(MBG_BUILD_TESTS   "Build the Microburst Governor test suite"      ON)
option(MBG_BUILD_TOOLS   "Build the mbgctl command line tool"            ON)
option(MBG_BUILD_BENCH   "Build the synthetic benchmark harness"         ON)
option(MBG_WARNINGS_AS_ERRORS "Promote first-party compiler warnings to errors" ON)
option(MBG_ENABLE_ASAN   "Enable AddressSanitizer when the toolchain supports it" OFF)
option(MBG_ENABLE_ANALYZE "Run MSVC /analyze as part of the build"       OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CTest)

# ---------------------------------------------------------------------------
# Warnings
# ---------------------------------------------------------------------------
add_library(mbg_warnings INTERFACE)
if(MSVC)
  target_compile_options(mbg_warnings INTERFACE
    /W4 /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor /EHsc /MP
    /wd4127)
  if(MBG_WARNINGS_AS_ERRORS)
    target_compile_options(mbg_warnings INTERFACE /WX)
  endif()
  if(MBG_ENABLE_ANALYZE)
    target_compile_options(mbg_warnings INTERFACE /analyze)
  endif()
else()
  target_compile_options(mbg_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Woverloaded-virtual -Wdouble-promotion)
  if(MBG_WARNINGS_AS_ERRORS)
    target_compile_options(mbg_warnings INTERFACE -Werror)
  endif()
endif()

# ---------------------------------------------------------------------------
# Core runtime library
# ---------------------------------------------------------------------------
set(MBG_SOURCES
  src/core/status.cpp
  src/core/checked.cpp
  src/core/crc32c.cpp
  src/core/version.cpp
  src/model/tick.cpp
  src/model/sample.cpp
  src/model/policy.cpp
  src/model/event.cpp
  src/model/intervention.cpp
  src/model/explanation.cpp
  src/model/severity.cpp
  src/detect/quantities.cpp
  src/detect/window.cpp
  src/detect/detector.cpp
  src/detect/classifier.cpp
  src/governor.cpp
  src/authority/fence.cpp
  src/persist/journal.cpp
  src/persist/snapshot.cpp
  src/transport/frame.cpp
  src/transport/socket.cpp
  src/coordinator/coordinator.cpp
  src/coordinator/worker.cpp
  src/platform/process.cpp
  src/synthetic/generator.cpp
  src/bench/benchmark.cpp
  src/cli/cli_common.cpp
  src/cli/command_replay.cpp
  src/cli/command_generate.cpp
  src/cli/command_verify.cpp
  src/cli/command_bench.cpp
  src/cli/command_selftest.cpp
)

add_library(mbg ${MBG_SOURCES})
add_library(mbg::mbg ALIAS mbg)
set_target_properties(mbg PROPERTIES
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  OUTPUT_NAME mbg)
target_include_directories(mbg
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(mbg PRIVATE MBG_BUILDING_LIBRARY=1)
# The warnings interface library is a build-only concern and is deliberately not exported.
target_link_libraries(mbg PRIVATE $<BUILD_INTERFACE:mbg_warnings>)

if(MSVC)
  target_compile_options(mbg PRIVATE /bigobj)
  # std::FILE is used deliberately for its explicit flush/commit semantics; the CRT "secure"
  # variants are not required and the deprecation warning is not actionable here.
  target_compile_definitions(mbg PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

if(MBG_ENABLE_ASAN)
  if(MSVC)
    target_compile_options(mbg PUBLIC /fsanitize=address)
    target_link_options(mbg PUBLIC /INCREMENTAL:NO)
  else()
    target_compile_options(mbg PUBLIC -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(mbg PUBLIC -fsanitize=address)
  endif()
endif()

if(WIN32)
  target_link_libraries(mbg PUBLIC ws2_32)
else()
  find_package(Threads REQUIRED)
  target_link_libraries(mbg PUBLIC Threads::Threads)
endif()

# ---------------------------------------------------------------------------
# CLI tool and benchmark harness
# ---------------------------------------------------------------------------
if(MBG_BUILD_TOOLS)
  add_executable(mbgctl src/cli/tool_main.cpp)
  target_link_libraries(mbgctl PRIVATE mbg mbg_warnings)
  set_target_properties(mbgctl PROPERTIES OUTPUT_NAME mbgctl)
endif()

if(MBG_BUILD_BENCH)
  add_executable(mbg_bench bench/bench_main.cpp)
  target_link_libraries(mbg_bench PRIVATE mbg mbg_warnings)
endif()

# ---------------------------------------------------------------------------
# Install / export
# ---------------------------------------------------------------------------
install(TARGETS mbg
  EXPORT mbgTargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
if(MBG_BUILD_TOOLS)
  install(TARGETS mbgctl RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

set(MBG_CMAKE_CONFIG_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/mbg")

install(EXPORT mbgTargets
  FILE mbg-targets.cmake
  NAMESPACE mbg::
  DESTINATION ${MBG_CMAKE_CONFIG_DESTINATION})

configure_package_config_file(
  cmake/mbg-config.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/mbg-config.cmake"
  INSTALL_DESTINATION ${MBG_CMAKE_CONFIG_DESTINATION})

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/mbg-config-version.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/mbg-config.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/mbg-config-version.cmake"
  DESTINATION ${MBG_CMAKE_CONFIG_DESTINATION})

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(MBG_BUILD_TESTS)
  add_subdirectory(tests)
endif()

message(STATUS "mbg ${PROJECT_VERSION}: tests=${MBG_BUILD_TESTS} tools=${MBG_BUILD_TOOLS} bench=${MBG_BUILD_BENCH} asan=${MBG_ENABLE_ASAN}")
