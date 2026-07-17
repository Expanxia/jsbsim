# Native (MSVC/host) JSBSim reference build for wasm-vs-native validation.
#   cmake -B build && cmake --build build --config Release
# The kiro-wasm hooks (JSBSIM_MEMVFS / JSBSIM_WASM_MINIMAL) are NOT defined
# here, which also verifies the guarded patches are no-ops for native builds.
cmake_minimum_required(VERSION 3.20)
project(jsbsim_native_ref CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(JSB_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../../src")

file(GLOB_RECURSE JSB_CPP "${JSB_SRC}/*.cpp" "${JSB_SRC}/*.cxx"
     "${JSB_SRC}/*.c")
list(FILTER JSB_CPP EXCLUDE REGEX "JSBSim\\.cpp$")
list(FILTER JSB_CPP EXCLUDE REGEX "JSBSim\\.minimal\\.cpp$")
list(FILTER JSB_CPP EXCLUDE REGEX "xmltok_impl\\.c$")
list(FILTER JSB_CPP EXCLUDE REGEX "xmltok_ns\\.c$")
list(FILTER JSB_CPP EXCLUDE REGEX "FGSimplexTrim\\.cpp$")
list(FILTER JSB_CPP EXCLUDE REGEX "FGTrimAnalysis.*\\.cpp$")

add_library(jsbcore_native STATIC ${JSB_CPP})
target_include_directories(jsbcore_native PUBLIC "${JSB_SRC}")
target_compile_definitions(jsbcore_native PUBLIC
  JSBSIM_VERSION="1.3.x-kiro-ref" JSBSIM_STATIC_LINK _USE_MATH_DEFINES
  NOMINMAX)
if(WIN32)
  target_compile_definitions(jsbcore_native PUBLIC XML_STATIC)
  target_link_libraries(jsbcore_native ws2_32)
endif()

add_executable(mini_ref mini_ref.cpp)
target_link_libraries(mini_ref jsbcore_native)
