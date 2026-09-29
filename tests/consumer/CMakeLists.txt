cmake_minimum_required(VERSION 3.21)

project(fco_consumer LANGUAGES CXX DESCRIPTION "Out-of-tree consumer of the installed Facility Change Orchestrator package")

# This project is deliberately built OUT OF TREE against an INSTALLED prefix. It
# must not see the source tree, the build tree, or the in-tree target: the only
# thing it may use is the exported fco::fco imported target.
find_package(fco CONFIG REQUIRED)

add_executable(fco_consumer main.cpp)
target_link_libraries(fco_consumer PRIVATE fco::fco)
set_target_properties(fco_consumer PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)

if(MSVC)
  target_compile_options(fco_consumer PRIVATE /W4 /WX /permissive- /Zc:__cplusplus /utf-8 /EHsc)
else()
  target_compile_options(fco_consumer PRIVATE -Wall -Wextra -Werror)
endif()
