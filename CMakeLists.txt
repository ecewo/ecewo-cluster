cmake_minimum_required(VERSION 3.14)
project(ecewo_cluster VERSION 0.1.0 LANGUAGES C)

add_library(ecewo_cluster
  src/ecewo-cluster.c
)

add_library(ecewo::cluster ALIAS ecewo_cluster)

target_include_directories(ecewo_cluster PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
)

if (PROJECT_IS_TOP_LEVEL AND NOT TARGET ecewo::ecewo)
  include(FetchContent)

  FetchContent_Declare(
    ecewo
    GIT_REPOSITORY https://github.com/savashn/ecewo.git
    GIT_TAG        v3.1.0
    GIT_SHALLOW    TRUE
  )

  FetchContent_MakeAvailable(ecewo)
endif()

target_compile_definitions(ecewo_cluster PRIVATE _GNU_SOURCE)

if (PROJECT_IS_TOP_LEVEL AND TARGET ecewo::ecewo)
  enable_testing()

  add_executable(ecewo_cluster_test
    tests/test.c
    tests/runner.c
  )

  target_include_directories(ecewo_cluster_test
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/src
      ${CMAKE_CURRENT_SOURCE_DIR}/tests
  )

  target_link_libraries(ecewo_cluster_test
    PRIVATE
      ecewo::ecewo
      ecewo::cluster
  )

  if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(ecewo_cluster_test PRIVATE
      -Wall
      -Wextra
      -Werror
      -Wstrict-prototypes
      -Wincompatible-pointer-types
      -Wno-unused-parameter
    )
  endif()

  add_test(
    NAME ecewo_cluster_all
    COMMAND ecewo_cluster_test
  )
endif()

target_link_libraries(ecewo_cluster PUBLIC ecewo::ecewo)
