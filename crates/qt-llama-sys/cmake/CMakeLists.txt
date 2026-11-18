# Wrapper project used by qt-llama-sys/build.rs.
#
# Builds the pinned llama.cpp fork as static libraries and adds a tiny C shim
# exposing the C++-only parts of `src/llama-ext.h` (device list and memory
# breakdown) through a C ABI that bindgen can bind.
cmake_minimum_required(VERSION 3.14)
project(qt_llama_wrapper C CXX)

if(NOT DEFINED QT_LLAMA_CPP_DIR)
    message(FATAL_ERROR "QT_LLAMA_CPP_DIR must point at the llama.cpp source tree")
endif()

add_subdirectory(${QT_LLAMA_CPP_DIR} llama.cpp)

add_library(qt_llama_shim STATIC qt_llama_shim.cpp)
target_include_directories(qt_llama_shim PRIVATE ${QT_LLAMA_CPP_DIR}/src)
target_link_libraries(qt_llama_shim PUBLIC llama)
target_compile_features(qt_llama_shim PRIVATE cxx_std_17)
