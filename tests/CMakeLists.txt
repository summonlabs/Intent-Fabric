add_executable(ifabric_tests
  main.cpp
  process.cpp
  test_core.cpp
  test_model.cpp
  test_canonical.cpp
  test_validate.cpp
  test_conflict.cpp
  test_diff.cpp
  test_store.cpp
  test_runtime.cpp
  test_property.cpp
  test_net.cpp
  test_cli.cpp
  test_hardening.cpp
)
target_include_directories(ifabric_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ifabric_tests PRIVATE ifabric_core)
ifabric_apply_warnings(ifabric_tests)
target_compile_definitions(ifabric_tests PRIVATE
  IFABRIC_CLI_PATH="$<TARGET_FILE:ifabric_cli>"
  IFABRICD_PATH="$<TARGET_FILE:ifabricd>")

if(WIN32)
  target_link_libraries(ifabric_tests PRIVATE ws2_32)
endif()

add_dependencies(ifabric_tests ifabric_cli ifabricd)

add_test(NAME ifabric_tests COMMAND ifabric_tests)

# Downstream packaging proof: install into a scratch prefix, then configure,
# build and run an independent consumer that only ever sees the installed
# artifact through find_package(IntentFabric).
add_test(NAME intentfabric_package_consumer
  COMMAND ${CMAKE_COMMAND}
    -DIFABRIC_SOURCE_DIR=${CMAKE_SOURCE_DIR}
    -DIFABRIC_BINARY_DIR=${CMAKE_BINARY_DIR}
    -DIFABRIC_CMAKE_COMMAND=${CMAKE_COMMAND}
    -DIFABRIC_GENERATOR=${CMAKE_GENERATOR}
    -DIFABRIC_BUILD_TYPE=${CMAKE_BUILD_TYPE}
    -DIFABRIC_CXX_COMPILER=${CMAKE_CXX_COMPILER}
    -DIFABRIC_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}
    -DIFABRIC_GENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}
    -P ${CMAKE_SOURCE_DIR}/cmake/RunDownstreamCheck.cmake)
