cmake_minimum_required(VERSION 3.16)

project(oms LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(OMS_ENABLE_AVX2 "Enable AVX2 SIMD" ON)
option(OMS_BUILD_DEV_TOOLS "Build standalone benchmark/tests" OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(OMS_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/include")

add_library(oms_core INTERFACE)
target_include_directories(oms_core INTERFACE ${OMS_INCLUDE_DIR})

if(MSVC)

    target_compile_options(oms_core INTERFACE
        /O2
        /fp:fast
    )

    if(OMS_ENABLE_AVX2)
        target_compile_options(oms_core INTERFACE /arch:AVX2)
    endif()

else()

    target_compile_options(oms_core INTERFACE
        -O3
        -ffast-math
    )

    if(OMS_ENABLE_AVX2)
        target_compile_options(oms_core INTERFACE
            -mavx2
            -march=native
        )
    endif()

endif()

find_package(Threads REQUIRED)

add_executable(oms
    src/oms_main.cpp
)

target_link_libraries(oms PRIVATE
    oms_core
    Threads::Threads
)

if(MINGW)

    target_link_options(oms PRIVATE
        -static
        -static-libgcc
        -static-libstdc++
    )

endif()

if(OMS_BUILD_DEV_TOOLS)

    add_executable(oms_test
        tests/test_order_book.cpp
    )

    target_link_libraries(oms_test PRIVATE
        oms_core
        Threads::Threads
    )

    if(MINGW)
        target_link_options(oms_test PRIVATE
            -static
            -static-libgcc
            -static-libstdc++
        )
    endif()

    add_executable(oms_benchmark
        benchmarks/book_benchmark.cpp
    )

    target_link_libraries(oms_benchmark PRIVATE
        oms_core
        Threads::Threads
    )

    if(MINGW)
        target_link_options(oms_benchmark PRIVATE
            -static
            -static-libgcc
            -static-libstdc++
        )
    endif()

    enable_testing()
    add_test(NAME order_book_tests COMMAND oms_test)

endif()

install(TARGETS oms RUNTIME DESTINATION bin)
