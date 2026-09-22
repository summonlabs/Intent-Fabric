# Intent Fabric - downstream packaging proof.
# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0.
#
# Installs the project into a scratch prefix, then configures, builds and runs
# an independent consumer that only ever sees the installed artifact through
# find_package(IntentFabric).

if(NOT DEFINED IFABRIC_SOURCE_DIR OR NOT DEFINED IFABRIC_BINARY_DIR)
  message(FATAL_ERROR "IFABRIC_SOURCE_DIR and IFABRIC_BINARY_DIR are required")
endif()

# Script mode does not enable a language, so the executable suffix is not set.
if(WIN32 AND NOT CMAKE_EXECUTABLE_SUFFIX)
  set(CMAKE_EXECUTABLE_SUFFIX ".exe")
endif()

set(prefix "${IFABRIC_BINARY_DIR}/package-prefix")
set(consumer_build "${IFABRIC_BINARY_DIR}/downstream-build")

file(REMOVE_RECURSE "${prefix}" "${consumer_build}")

execute_process(
  COMMAND "${IFABRIC_CMAKE_COMMAND}" --install "${IFABRIC_BINARY_DIR}" --prefix "${prefix}"
  RESULT_VARIABLE install_result OUTPUT_VARIABLE install_out ERROR_VARIABLE install_err)
if(NOT install_result EQUAL 0)
  message(FATAL_ERROR "cmake --install failed:\n${install_out}\n${install_err}")
endif()

set(configure_args
  -S "${IFABRIC_SOURCE_DIR}/tests/downstream"
  -B "${consumer_build}"
  "-DCMAKE_PREFIX_PATH=${prefix}")
if(DEFINED IFABRIC_GENERATOR AND NOT IFABRIC_GENERATOR STREQUAL "")
  list(APPEND configure_args -G "${IFABRIC_GENERATOR}")
endif()
if(DEFINED IFABRIC_BUILD_TYPE AND NOT IFABRIC_BUILD_TYPE STREQUAL "")
  list(APPEND configure_args "-DCMAKE_BUILD_TYPE=${IFABRIC_BUILD_TYPE}")
endif()
if(DEFINED IFABRIC_CXX_COMPILER AND NOT IFABRIC_CXX_COMPILER STREQUAL "")
  list(APPEND configure_args "-DCMAKE_CXX_COMPILER=${IFABRIC_CXX_COMPILER}")
endif()
if(DEFINED IFABRIC_GENERATOR_PLATFORM AND NOT IFABRIC_GENERATOR_PLATFORM STREQUAL "")
  list(APPEND configure_args "-A" "${IFABRIC_GENERATOR_PLATFORM}")
endif()
if(DEFINED IFABRIC_GENERATOR_TOOLSET AND NOT IFABRIC_GENERATOR_TOOLSET STREQUAL "")
  list(APPEND configure_args "-T" "${IFABRIC_GENERATOR_TOOLSET}")
endif()

execute_process(
  COMMAND "${IFABRIC_CMAKE_COMMAND}" ${configure_args}
  RESULT_VARIABLE configure_result OUTPUT_VARIABLE configure_out ERROR_VARIABLE configure_err)
if(NOT configure_result EQUAL 0)
  message(FATAL_ERROR "downstream configure failed:\n${configure_out}\n${configure_err}")
endif()

execute_process(
  COMMAND "${IFABRIC_CMAKE_COMMAND}" --build "${consumer_build}"
  RESULT_VARIABLE build_result OUTPUT_VARIABLE build_out ERROR_VARIABLE build_err)
if(NOT build_result EQUAL 0)
  message(FATAL_ERROR "downstream build failed:\n${build_out}\n${build_err}")
endif()

set(consumer_exe "${consumer_build}/bin/ifabric_downstream_consumer${CMAKE_EXECUTABLE_SUFFIX}")
if(NOT EXISTS "${consumer_exe}")
  # Multi-configuration generators add a configuration subdirectory.
  file(GLOB candidates "${consumer_build}/bin/*/ifabric_downstream_consumer${CMAKE_EXECUTABLE_SUFFIX}")
  list(LENGTH candidates candidate_count)
  if(candidate_count GREATER 0)
    list(GET candidates 0 consumer_exe)
  endif()
endif()
if(NOT EXISTS "${consumer_exe}")
  message(FATAL_ERROR "downstream consumer executable was not produced under ${consumer_build}/bin")
endif()

execute_process(
  COMMAND "${consumer_exe}"
  RESULT_VARIABLE run_result OUTPUT_VARIABLE run_out ERROR_VARIABLE run_err)
if(NOT run_result EQUAL 0)
  message(FATAL_ERROR "downstream consumer failed (${run_result}):\n${run_out}\n${run_err}")
endif()

message(STATUS "downstream find_package consumer: ${run_out}")
