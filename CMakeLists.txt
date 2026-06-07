cmake_minimum_required(VERSION 3.16)
project(oms LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(OMS_ENABLE_AVX2 "Enable AVX2 SIMD for OBI computation" ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(MSVC)
    add_compile_options(/O2 /fp:fast)
    if(OMS_ENABLE_AVX2)
        add_compile_options(/arch:AVX2)
    endif()
else()
    add_compile_options(-O3 -ffast-math)
    if(OMS_ENABLE_AVX2)
        add_compile_options(-mavx2 -march=native)
    endif()
endif()

set(OMS_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/include")

# Header-only library interface
add_library(oms_core INTERFACE)
target_include_directories(oms_core INTERFACE ${OMS_INCLUDE_DIR})

# Tests
add_executable(oms_test tests/test_order_book.cpp)
target_link_libraries(oms_test PRIVATE oms_core)

# Benchmarks
add_executable(oms_benchmark benchmarks/book_benchmark.cpp)
target_link_libraries(oms_benchmark PRIVATE oms_core)

# Primary product — single interactive executable with visuals + HTML report
add_executable(oms src/oms_main.cpp)
target_link_libraries(oms PRIVATE oms_core)
if(UNIX AND NOT APPLE)
    target_link_libraries(oms PRIVATE pthread)
elseif(MINGW)
    target_link_libraries(oms PRIVATE pthread)
endif()

option(OMS_BUILD_LEGACY_DEMOS "Build standalone demo executables" OFF)
if(OMS_BUILD_LEGACY_DEMOS)
    add_executable(oms_demo src/demo.cpp)
    target_link_libraries(oms_demo PRIVATE oms_core)
    add_executable(oms_matching_demo src/matching_demo.cpp)
    target_link_libraries(oms_matching_demo PRIVATE oms_core)
    if(UNIX AND NOT APPLE)
        target_link_libraries(oms_matching_demo PRIVATE pthread)
    elseif(MINGW)
        target_link_libraries(oms_matching_demo PRIVATE pthread)
    endif()
endif()

install(TARGETS oms RUNTIME DESTINATION bin)

enable_testing()
add_test(NAME order_book_tests COMMAND oms_test)
