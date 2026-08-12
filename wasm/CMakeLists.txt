# pree-wasm: builds jsbsim.wasm — a standalone WASI reactor module exposing
# the flat C ABI in abi.h. Configure with the wasi-sdk toolchain file:
#   cmake -B build -G Ninja \
#     -DCMAKE_TOOLCHAIN_FILE=<wasi-sdk>/share/cmake/wasi-sdk.cmake \
#     -DCMAKE_BUILD_TYPE=Release
# (or just run ./build.sh). LTO stays OFF: known wasi-sdk issues with
# -flto + -fwasm-exceptions.
cmake_minimum_required(VERSION 3.20)
project(jsbsim_kiro_wasm CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(JSB_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../src")

file(GLOB_RECURSE JSB_CPP "${JSB_SRC}/*.cpp" "${JSB_SRC}/*.cxx"
     "${JSB_SRC}/*.c")

# CLI mains and expat's #include-only translation units.
list(FILTER JSB_CPP EXCLUDE REGEX "JSBSim\\.cpp$")
list(FILTER JSB_CPP EXCLUDE REGEX "JSBSim\\.minimal\\.cpp$")
list(FILTER JSB_CPP EXCLUDE REGEX "xmltok_impl\\.c$")
list(FILTER JSB_CPP EXCLUDE REGEX "xmltok_ns\\.c$")
# Dead sources not present in the official build.
list(FILTER JSB_CPP EXCLUDE REGEX "FGSimplexTrim\\.cpp$")
list(FILTER JSB_CPP EXCLUDE REGEX "FGTrimAnalysis.*\\.cpp$")
# JSBSIM_WASM_MINIMAL: sockets cannot exist on wasi-libc. The matching
# dispatch guards live in FGOutput.cpp / FGInput.cpp.
list(FILTER JSB_CPP EXCLUDE REGEX "FGfdmSocket\\.cpp$")
list(FILTER JSB_CPP EXCLUDE REGEX "FGOutputSocket\\.cpp$")
list(FILTER JSB_CPP EXCLUDE REGEX "FGOutputFG\\.cpp$")
list(FILTER JSB_CPP EXCLUDE REGEX "FGInputSocket\\.cpp$")
list(FILTER JSB_CPP EXCLUDE REGEX "FGUDPInputSocket\\.cpp$")

set(EH_FLAGS -fwasm-exceptions -mllvm -wasm-use-legacy-eh=false)

add_library(jsbcore STATIC ${JSB_CPP})
target_include_directories(jsbcore PUBLIC "${JSB_SRC}")
target_compile_definitions(jsbcore PUBLIC
  JSBSIM_VERSION="1.3.x-pree-wasm"
  JSBSIM_MEMVFS
  JSBSIM_WASM_MINIMAL)
target_compile_options(jsbcore PUBLIC ${EH_FLAGS})

add_executable(jsbsim_wasm
  facade.cpp
  memvfs.cpp
  ground_host.cpp)
target_link_libraries(jsbsim_wasm jsbcore)
set_target_properties(jsbsim_wasm PROPERTIES OUTPUT_NAME "jsbsim"
                      SUFFIX ".wasm")
target_link_options(jsbsim_wasm PRIVATE
  -mexec-model=reactor
  -fwasm-exceptions
  -Wl,-mllvm,-wasm-use-legacy-eh=false
  -lunwind)
