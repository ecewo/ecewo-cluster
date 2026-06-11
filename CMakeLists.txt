cmake_minimum_required(VERSION 3.14)
project(ecewo_cluster VERSION 0.2.0 LANGUAGES C)

option(ECEWO_CLUSTER_BUILD_SHARED "Build ecewo-cluster as a shared library" OFF)

# When building shared, all (transitively static) deps must be PIC-compatible.
if(ECEWO_CLUSTER_BUILD_SHARED)
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

if(ECEWO_CLUSTER_BUILD_SHARED)
  add_library(ecewo_cluster SHARED src/ecewo-cluster.c)
  message(STATUS "Building ecewo-cluster as SHARED library")
else()
  add_library(ecewo_cluster STATIC src/ecewo-cluster.c)
  message(STATUS "Building ecewo-cluster as STATIC library")
endif()

include(GenerateExportHeader)
generate_export_header(ecewo_cluster
  BASE_NAME ECEWO_CLUSTER
  EXPORT_FILE_NAME "${CMAKE_CURRENT_BINARY_DIR}/include/ecewo-cluster-export.h"
)

set_target_properties(ecewo_cluster PROPERTIES
  C_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
)

if(NOT ECEWO_CLUSTER_BUILD_SHARED)
  target_compile_definitions(ecewo_cluster PUBLIC ECEWO_CLUSTER_STATIC_DEFINE)
endif()

add_library(ecewo::cluster ALIAS ecewo_cluster)

target_include_directories(ecewo_cluster PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
)

if (PROJECT_IS_TOP_LEVEL AND NOT TARGET ecewo::ecewo)
  include(FetchContent)

  FetchContent_Declare(
    ecewo
    GIT_REPOSITORY https://github.com/ecewo/ecewo.git
    GIT_TAG v4.0.0
    GIT_SHALLOW TRUE
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
