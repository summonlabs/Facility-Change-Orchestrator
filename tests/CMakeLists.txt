# Test suite registration.
#
# Every test target is a first-party target and therefore builds under the same
# strict warning policy as the library. No test is registered with a timeout:
# a hanging test is a defect to diagnose, not a condition to tolerate.

set(FCO_UNIT_TESTS
  test_error
  test_digest
  test_strong
  test_domain
  test_authority
  test_facility
  test_model
  test_json
  test_codec
  test_store
  test_planner
  test_evidence
  test_engine
  test_property
  test_adversarial
)

foreach(test_name IN LISTS FCO_UNIT_TESTS)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${test_name}.cpp")
    add_executable(${test_name} ${test_name}.cpp)
    target_link_libraries(${test_name} PRIVATE fco::fco)
    target_include_directories(${test_name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    fco_warnings(${test_name})
    add_test(NAME ${test_name} COMMAND ${test_name})
  endif()
endforeach()

# Multiprocess, crash-consistency and restart tests. These spawn real operating
# system processes and therefore need the built executable paths.
add_executable(test_process test_process.cpp)
target_link_libraries(test_process PRIVATE fco::fco)
target_include_directories(test_process PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
fco_warnings(test_process)
add_test(NAME test_process COMMAND test_process)
set_tests_properties(test_process PROPERTIES ENVIRONMENT "FCO_CLI=$<TARGET_FILE:fco_cli>;FCO_CRASH_HARNESS=$<TARGET_FILE:fco_crash_harness>;FCO_STORE_PROBE=$<TARGET_FILE:fco_store_probe>")

if(TARGET fco_cli)
  add_executable(fco_crash_harness crash_harness.cpp)
  target_link_libraries(fco_crash_harness PRIVATE fco::fco)
  target_include_directories(fco_crash_harness PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  fco_warnings(fco_crash_harness)

  add_executable(fco_store_probe store_probe.cpp)
  target_link_libraries(fco_store_probe PRIVATE fco::fco)
  target_include_directories(fco_store_probe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  fco_warnings(fco_store_probe)
endif()

# Installed-artifact consumer. Built out of tree by scripts/validate_package.ps1
# against the installed package; registered here only so the sources are part of
# the repository.
