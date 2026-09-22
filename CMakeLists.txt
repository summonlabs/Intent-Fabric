cmake_minimum_required(VERSION 3.20)

project(IntentFabric
  VERSION 1.0.0
  DESCRIPTION "Intent Fabric - authoritative desired-state intent runtime (Summon Software Labs Fabric OS)"
  HOMEPAGE_URL "https://github.com/summonlabs/Intent-Fabric"
  LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(IFABRIC_BUILD_TOOLS    "Build the ifabric / ifabricd command line tools" ON)
option(IFABRIC_BUILD_TESTS    "Build the Intent Fabric test suites"             ON)
option(IFABRIC_BUILD_EXAMPLES "Build the Intent Fabric examples"                ON)
option(IFABRIC_BUILD_BENCH    "Build the Intent Fabric benchmarks"              ON)
option(IFABRIC_ENABLE_SANITIZERS "Enable Address/Undefined sanitizers (GCC/Clang)" OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# ---------------------------------------------------------------------------
# Warnings: zero first-party warnings is a hard requirement.
#
# The warning set is applied directly to first-party targets so that it is
# never exported to downstream consumers through the installed package.
# ---------------------------------------------------------------------------
set(IFABRIC_WARNING_OPTIONS "")
if(MSVC)
  set(IFABRIC_WARNING_OPTIONS
    /W4 /WX /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor /EHsc
    /w14242 /w14254 /w14263 /w14265 /w14287 /w14296 /w14311 /w14545 /w14546
    /w14547 /w14548 /w14549 /w14555 /w14619 /w14640 /w14826 /w14905 /w14906
    /w14928 /w15038 /wd4127)
else()
  set(IFABRIC_WARNING_OPTIONS
    -Wall -Wextra -Wpedantic -Werror -Wshadow -Wnon-virtual-dtor
    -Wold-style-cast -Wcast-align -Wunused -Woverloaded-virtual -Wconversion
    -Wsign-conversion -Wdouble-promotion -Wformat=2 -Wundef
    -Wno-unknown-warning-option)
endif()

function(ifabric_apply_warnings target)
  target_compile_options(${target} PRIVATE ${IFABRIC_WARNING_OPTIONS})
  if(IFABRIC_ENABLE_SANITIZERS AND NOT MSVC)
    target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(${target} PRIVATE -fsanitize=address,undefined)
  endif()
endfunction()

# ---------------------------------------------------------------------------
# Core library
# ---------------------------------------------------------------------------
add_library(ifabric_core STATIC
  src/core.cpp
  src/types.cpp
  src/json.cpp
  src/model.cpp
  src/model_json.cpp
  src/canonical.cpp
  src/index.cpp
  src/validate.cpp
  src/conflict.cpp
  src/diff.cpp
  src/io.cpp
  src/store.cpp
  src/runtime.cpp
  src/net.cpp
  src/synth.cpp
  src/cli.cpp
)
add_library(IntentFabric::ifabric ALIAS ifabric_core)

set_target_properties(ifabric_core PROPERTIES
  OUTPUT_NAME ifabric
  EXPORT_NAME ifabric
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  POSITION_INDEPENDENT_CODE ON)

target_include_directories(ifabric_core
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)

ifabric_apply_warnings(ifabric_core)
target_compile_definitions(ifabric_core PUBLIC
  IFABRIC_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
  IFABRIC_VERSION_MINOR=${PROJECT_VERSION_MINOR}
  IFABRIC_VERSION_PATCH=${PROJECT_VERSION_PATCH}
  IFABRIC_VERSION_STRING="${PROJECT_VERSION}")

if(WIN32)
  target_link_libraries(ifabric_core PUBLIC ws2_32)
endif()

if(UNIX AND NOT APPLE)
  find_package(Threads REQUIRED)
  target_link_libraries(ifabric_core PUBLIC Threads::Threads)
endif()

# ---------------------------------------------------------------------------
# CLI tools
# ---------------------------------------------------------------------------
if(IFABRIC_BUILD_TOOLS)
  add_executable(ifabric_cli tools/ifabric_main.cpp)
  set_target_properties(ifabric_cli PROPERTIES OUTPUT_NAME ifabric)
  target_link_libraries(ifabric_cli PRIVATE ifabric_core)
  ifabric_apply_warnings(ifabric_cli)

  add_executable(ifabricd tools/ifabricd_main.cpp)
  target_link_libraries(ifabricd PRIVATE ifabric_core)
  ifabric_apply_warnings(ifabricd)

  install(TARGETS ifabric_cli ifabricd
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    COMPONENT tools)
endif()

# ---------------------------------------------------------------------------
# Install / export
# ---------------------------------------------------------------------------
install(TARGETS ifabric_core
  EXPORT IntentFabricTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT runtime
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT runtime
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT runtime
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} COMPONENT development)

install(EXPORT IntentFabricTargets
  FILE IntentFabricTargets.cmake
  NAMESPACE IntentFabric::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/IntentFabric
  COMPONENT development)

configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/IntentFabricConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/IntentFabricConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/IntentFabric)

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/IntentFabricConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/IntentFabricConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/IntentFabricConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/IntentFabric
  COMPONENT development)

install(FILES LICENSE README.md
  DESTINATION ${CMAKE_INSTALL_DATADIR}/IntentFabric
  COMPONENT development)

# ---------------------------------------------------------------------------
# Examples / Tests / Benchmarks
# ---------------------------------------------------------------------------
if(IFABRIC_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(IFABRIC_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(IFABRIC_BUILD_BENCH)
  add_subdirectory(bench)
endif()

message(STATUS "Intent Fabric ${PROJECT_VERSION} | build type: ${CMAKE_BUILD_TYPE} | compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
