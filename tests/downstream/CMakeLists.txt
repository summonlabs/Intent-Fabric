cmake_minimum_required(VERSION 3.20)

project(IntentFabricDownstreamConsumer
  VERSION 1.0.0
  DESCRIPTION "Independent downstream consumer of the installed Intent Fabric package"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(IntentFabric 1.0 REQUIRED)

add_executable(ifabric_downstream_consumer main.cpp)
target_link_libraries(ifabric_downstream_consumer PRIVATE IntentFabric::ifabric)

set_target_properties(ifabric_downstream_consumer PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
  RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin"
  RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
  RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/bin")
