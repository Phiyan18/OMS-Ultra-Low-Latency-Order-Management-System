cmake_minimum_required(VERSION 3.16)

project(oms LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(OMS_ENABLE_AVX2 "Enable AVX2 SIMD" ON)
option(OMS_BUILD_DEV_TOOLS "Build standalone benchmark/tests" OFF)
option(OMS_FETCH_GUI_DEPS "Download pinned GUI dependencies when they are not installed" ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(OMS_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/include")

add_library(oms_core INTERFACE)
target_include_directories(oms_core INTERFACE ${OMS_INCLUDE_DIR})

if(MSVC)
    target_compile_options(oms_core INTERFACE /O2 /fp:fast)
    if(OMS_ENABLE_AVX2)
        target_compile_options(oms_core INTERFACE /arch:AVX2)
    endif()
else()
    target_compile_options(oms_core INTERFACE -O3 -ffast-math)
    if(OMS_ENABLE_AVX2)
        target_compile_options(oms_core INTERFACE -mavx2 -march=native)
    endif()
endif()

find_package(Threads REQUIRED)

set(OMS_SOURCES
    src/oms_main.cpp
    src/workstation.cpp
)

find_package(OpenGL QUIET)
find_package(glfw3 QUIET)
find_path(OMS_IMGUI_INCLUDE_DIR imgui.h)
find_path(OMS_IMGUI_BACKENDS_DIR imgui_impl_glfw.h PATH_SUFFIXES backends)
find_path(OMS_IMPLOT_INCLUDE_DIR implot.h)
find_path(OMS_IMNODES_INCLUDE_DIR imnodes.h)
find_library(OMS_IMGUI_LIBRARY NAMES imgui)
find_library(OMS_IMPLOT_LIBRARY NAMES implot)
find_library(OMS_IMNODES_LIBRARY NAMES imnodes)

if(OMS_FETCH_GUI_DEPS AND NOT (OpenGL_FOUND AND OMS_IMGUI_INCLUDE_DIR AND
   OMS_IMGUI_BACKENDS_DIR AND OMS_IMPLOT_INCLUDE_DIR AND OMS_IMNODES_INCLUDE_DIR AND
   OMS_IMGUI_LIBRARY AND OMS_IMPLOT_LIBRARY AND OMS_IMNODES_LIBRARY))
    include(FetchContent)
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    # URL archives avoid requiring git on PATH (common on Windows).
    FetchContent_Declare(oms_glfw
        URL https://github.com/glfw/glfw/archive/refs/tags/3.4.zip)
    FetchContent_Declare(oms_imgui_src
        URL https://github.com/ocornut/imgui/archive/refs/heads/docking.zip)
    FetchContent_Declare(oms_implot_src
        URL https://github.com/epezent/implot/archive/refs/tags/v0.17.zip)
    FetchContent_Declare(oms_imnodes_src
        URL https://github.com/Nelarius/imnodes/archive/refs/heads/master.zip)
    FetchContent_MakeAvailable(oms_glfw oms_imgui_src oms_implot_src)
    FetchContent_GetProperties(oms_imnodes_src)
    if(NOT oms_imnodes_src_POPULATED)
        FetchContent_Populate(oms_imnodes_src)
    endif()

    add_library(oms_imgui STATIC
        ${oms_imgui_src_SOURCE_DIR}/imgui.cpp
        ${oms_imgui_src_SOURCE_DIR}/imgui_draw.cpp
        ${oms_imgui_src_SOURCE_DIR}/imgui_tables.cpp
        ${oms_imgui_src_SOURCE_DIR}/imgui_widgets.cpp
        ${oms_imgui_src_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
        ${oms_imgui_src_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
    target_include_directories(oms_imgui PUBLIC
        ${oms_imgui_src_SOURCE_DIR}
        ${oms_imgui_src_SOURCE_DIR}/backends)
    target_compile_definitions(oms_imgui PUBLIC IMGUI_DEFINE_MATH_OPERATORS)
    target_link_libraries(oms_imgui PUBLIC glfw)

    add_library(oms_implot STATIC
        ${oms_implot_src_SOURCE_DIR}/implot.cpp
        ${oms_implot_src_SOURCE_DIR}/implot_items.cpp)
    target_include_directories(oms_implot PUBLIC ${oms_implot_src_SOURCE_DIR})
    target_link_libraries(oms_implot PUBLIC oms_imgui)

    add_library(oms_imnodes STATIC ${oms_imnodes_src_SOURCE_DIR}/imnodes.cpp)
    target_include_directories(oms_imnodes PUBLIC ${oms_imnodes_src_SOURCE_DIR})
    target_compile_definitions(oms_imnodes PRIVATE IMGUI_DEFINE_MATH_OPERATORS)
    target_link_libraries(oms_imnodes PUBLIC oms_imgui)

    find_package(OpenGL REQUIRED)
    set(OMS_IMGUI_INCLUDE_DIR ${oms_imgui_src_SOURCE_DIR})
    set(OMS_IMGUI_BACKENDS_DIR ${oms_imgui_src_SOURCE_DIR}/backends)
    set(OMS_IMPLOT_INCLUDE_DIR ${oms_implot_src_SOURCE_DIR})
    set(OMS_IMNODES_INCLUDE_DIR ${oms_imnodes_src_SOURCE_DIR})
    set(OMS_IMGUI_LIBRARY oms_imgui)
    set(OMS_IMPLOT_LIBRARY oms_implot)
    set(OMS_IMNODES_LIBRARY oms_imnodes)
    set(OMS_GLFW_TARGET glfw)
endif()

if(NOT OMS_GLFW_TARGET)
    if(TARGET glfw3::glfw)
        set(OMS_GLFW_TARGET glfw3::glfw)
    elseif(TARGET glfw)
        set(OMS_GLFW_TARGET glfw)
    endif()
endif()

if(NOT (OpenGL_FOUND AND OMS_GLFW_TARGET AND OMS_IMGUI_INCLUDE_DIR AND
   OMS_IMGUI_BACKENDS_DIR AND OMS_IMPLOT_INCLUDE_DIR AND OMS_IMNODES_INCLUDE_DIR AND
   OMS_IMGUI_LIBRARY AND OMS_IMPLOT_LIBRARY AND OMS_IMNODES_LIBRARY))
    message(FATAL_ERROR
        "GUI dependencies are required for OMS.\n"
        "Install Dear ImGui, ImPlot, ImNodes, GLFW, and OpenGL, or configure with:\n"
        "FetchContent requires network access (or install system packages).\n"
        "Reconfigure with a fresh build directory: cmake -B build -DOMS_FETCH_GUI_DEPS=ON")
endif()

if(NOT TARGET oms_imgui)
    list(APPEND OMS_SOURCES
        "${OMS_IMGUI_BACKENDS_DIR}/imgui_impl_glfw.cpp"
        "${OMS_IMGUI_BACKENDS_DIR}/imgui_impl_opengl3.cpp"
    )
endif()

add_executable(oms ${OMS_SOURCES})

target_include_directories(oms PRIVATE
    ${OMS_IMGUI_INCLUDE_DIR}
    ${OMS_IMGUI_BACKENDS_DIR}
    ${OMS_IMPLOT_INCLUDE_DIR}
    ${OMS_IMNODES_INCLUDE_DIR}
)

target_link_libraries(oms PRIVATE
    oms_core
    Threads::Threads
    ${OMS_IMGUI_LIBRARY}
    ${OMS_IMPLOT_LIBRARY}
    ${OMS_IMNODES_LIBRARY}
    ${OMS_GLFW_TARGET}
    OpenGL::GL
)

if(MINGW)
    target_link_options(oms PRIVATE -static -static-libgcc -static-libstdc++)
endif()

if(OMS_BUILD_DEV_TOOLS)
    add_executable(oms_test tests/test_order_book.cpp)
    target_link_libraries(oms_test PRIVATE oms_core Threads::Threads)
    if(MINGW)
        target_link_options(oms_test PRIVATE -static -static-libgcc -static-libstdc++)
    endif()

    add_executable(oms_benchmark benchmarks/book_benchmark.cpp)
    target_link_libraries(oms_benchmark PRIVATE oms_core Threads::Threads)
    if(MINGW)
        target_link_options(oms_benchmark PRIVATE -static -static-libgcc -static-libstdc++)
    endif()

    enable_testing()
    add_test(NAME order_book_tests COMMAND oms_test)
endif()

install(TARGETS oms RUNTIME DESTINATION bin)
