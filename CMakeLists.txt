cmake_minimum_required(VERSION 3.16)
project(GenesisUI VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- Options ---
option(GENESIS_BUILD_EXAMPLES "Build example applications" ON)
option(GENESIS_BUILD_TESTS "Build validation tests" ON)

# --- Dependencies ---
find_package(PkgConfig REQUIRED)
if(UNIX AND NOT APPLE)
    pkg_check_modules(XCB REQUIRED xcb)
endif()

# --- Core Library ---
add_library(genesis_core INTERFACE)
target_include_directories(genesis_core INTERFACE 
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# --- Platform Implementation ---
add_library(genesis_platform STATIC)
target_link_libraries(genesis_platform PUBLIC genesis_core)

if(UNIX AND NOT APPLE)
    target_sources(genesis_platform PRIVATE src/platforms/linux/LinuxPlatformWindow.cpp)
    target_include_directories(genesis_platform PRIVATE ${XCB_INCLUDE_DIRS})
    target_link_libraries(genesis_platform PRIVATE ${XCB_LIBRARIES})
endif()

# --- Graphics Backend Factory ---
# For now, GpuHal.cpp provides the factory implementation
target_sources(genesis_platform PRIVATE src/graphics/GpuHal.cpp)

# --- Examples ---
if(GENESIS_BUILD_EXAMPLES)
    add_executable(genesis_performance_panel examples/performance_panel/main.cpp)
    target_link_libraries(genesis_performance_panel PRIVATE genesis_platform)
endif()

# --- Tests ---
if(GENESIS_BUILD_TESTS)
    enable_testing()
    
    function(add_genesis_test name src)
        add_executable(${name} ${src})
        target_link_libraries(${name} PRIVATE genesis_platform)
        add_test(NAME ${name} COMMAND ${name})
    endfunction()

    add_genesis_test(test_signal src/tests/test_signal.cpp)
    add_genesis_test(test_application src/tests/test_application.cpp)
    add_genesis_test(test_scenegraph src/tests/test_scenegraph.cpp)
    add_genesis_test(test_primitives src/tests/test_primitives.cpp)
    add_genesis_test(test_gpuhal src/tests/test_gpuhal.cpp)
    add_genesis_test(test_aicontrolbus src/tests/test_aicontrolbus.cpp)
    add_genesis_test(test_widgets src/tests/test_widgets.cpp)
    add_genesis_test(test_fontengine src/tests/test_fontengine.cpp)
    add_genesis_test(test_style src/tests/test_style.cpp)
    add_genesis_test(test_invalidation src/tests/test_invalidation.cpp)
    add_genesis_test(test_assets src/tests/test_assets.cpp)
endif()
