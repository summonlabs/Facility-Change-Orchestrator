cmake_minimum_required(VERSION 3.21)

project(FacilityChangeOrchestrator
        VERSION 1.0.0
        DESCRIPTION "Facility Change Orchestrator: DCCP physical fleet lifecycle change planning and orchestration runtime"
        HOMEPAGE_URL "https://github.com/summonlabs/Facility-Change-Orchestrator"
        LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(FCO_BUILD_TESTS "Build the Facility Change Orchestrator test suite" ON)
option(FCO_BUILD_CLI "Build the fco command line runtime" ON)
option(FCO_BUILD_BENCHMARKS "Build the fco benchmark harness" OFF)
option(FCO_WARNINGS_AS_ERRORS "Treat first-party compiler warnings as errors" ON)
option(FCO_ENABLE_SANITIZERS "Enable AddressSanitizer/UndefinedBehaviorSanitizer (non-MSVC)" OFF)

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Release Debug RelWithDebInfo)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

# ---------------------------------------------------------------------------
# Strict first-party warnings. Never applied to third-party or generated code.
# ---------------------------------------------------------------------------
function(fco_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE
      /W4 /permissive- /Zc:__cplusplus /Zc:preprocessor /Zc:inline /utf-8 /EHsc)
    target_compile_definitions(${target} PRIVATE
      NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
    if(FCO_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE
      -Wall -Wextra -Wpedantic -Wshadow -Wold-style-cast -Wcast-qual -Wuseless-cast
      -Wnon-virtual-dtor -Woverloaded-virtual -Wdouble-promotion -Wformat=2)
    target_compile_definitions(${target} PRIVATE _FILE_OFFSET_BITS=64)
    if(FCO_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
    if(FCO_ENABLE_SANITIZERS)
      target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
      target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()
  endif()
endfunction()

set(FCO_LIBRARY_SOURCES
  src/error.cpp
  src/digest.cpp
  src/json.cpp
  src/strong.cpp
  src/domain.cpp
  src/authority.cpp
  src/facility.cpp
  src/model.cpp
  src/planner.cpp
  src/evidence.cpp
  src/codec.cpp
  src/state.cpp
  src/ports.cpp
  src/store.cpp
  src/engine.cpp
  src/render.cpp
  src/sim.cpp
  src/cli.cpp
)

add_library(fco STATIC ${FCO_LIBRARY_SOURCES})
add_library(fco::fco ALIAS fco)
add_library(SummonSoftwareLabs::fco ALIAS fco)

target_include_directories(fco
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")

target_compile_features(fco PUBLIC cxx_std_20)
set_target_properties(fco PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
  POSITION_INDEPENDENT_CODE ON)
fco_warnings(fco)

if(FCO_BUILD_CLI)
  add_executable(fco_cli cli/main.cpp)
  target_link_libraries(fco_cli PRIVATE fco::fco)
  set_target_properties(fco_cli PROPERTIES OUTPUT_NAME fco)
  fco_warnings(fco_cli)
endif()

if(FCO_BUILD_BENCHMARKS)
  add_executable(fco_bench bench/bench_main.cpp)
  target_link_libraries(fco_bench PRIVATE fco::fco)
  fco_warnings(fco_bench)
endif()

if(FCO_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Install / export / package
# ---------------------------------------------------------------------------
install(TARGETS fco
  EXPORT fcoTargets
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

if(FCO_BUILD_CLI)
  install(TARGETS fco_cli RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif()

install(EXPORT fcoTargets
  NAMESPACE fco::
  FILE fcoTargets.cmake
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/fco")

configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/fcoConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/fcoConfig.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/fco")

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/fcoConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/fcoConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/fcoConfigVersion.cmake"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/fco")

install(FILES LICENSE NOTICE README.md
  DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/fco")

set(CPACK_PACKAGE_NAME "FacilityChangeOrchestrator")
set(CPACK_PACKAGE_VENDOR "Summon Software Labs")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "DCCP physical fleet lifecycle change planning and orchestration runtime")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_GENERATOR "ZIP")
set(CPACK_PACKAGE_FILE_NAME "FacilityChangeOrchestrator-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
include(CPack)
