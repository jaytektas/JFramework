cmake_minimum_required(VERSION 3.16)
project(GenesisUI VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- Options ---
option(GENESIS_BUILD_EXAMPLES "Build example applications" ON)
option(GENESIS_BUILD_TESTS    "Build validation tests"     ON)

# --- Shader compilation ---
find_program(GLSLC_EXECUTABLE NAMES glslc)
find_package(Python3 COMPONENTS Interpreter QUIET)

if(GLSLC_EXECUTABLE AND Python3_Interpreter_FOUND)
    set(SHADER_DIR    ${CMAKE_SOURCE_DIR}/src/shaders)
    set(SHADER_BUILD  ${CMAKE_BINARY_DIR}/shaders)
    set(SHADER_HEADER ${CMAKE_SOURCE_DIR}/include/genesis/graphics/ShaderSpirv.h)
    set(EMBED_SCRIPT  ${CMAKE_SOURCE_DIR}/cmake/embed_spv.py)
    file(MAKE_DIRECTORY ${SHADER_BUILD})

    foreach(SH rect.vert rect.frag text.vert text.frag image.vert image.frag)
        add_custom_command(
            OUTPUT  ${SHADER_BUILD}/${SH}.spv
            COMMAND ${GLSLC_EXECUTABLE} ${SHADER_DIR}/${SH} -o ${SHADER_BUILD}/${SH}.spv
            DEPENDS ${SHADER_DIR}/${SH}
            VERBATIM COMMENT "Compiling ${SH} → SPIR-V"
        )
    endforeach()

    add_custom_command(
        OUTPUT  ${SHADER_HEADER}
        COMMAND ${Python3_EXECUTABLE} ${EMBED_SCRIPT}
                ${SHADER_BUILD}/rect.vert.spv ${SHADER_BUILD}/rect.frag.spv
                ${SHADER_BUILD}/text.vert.spv ${SHADER_BUILD}/text.frag.spv
                ${SHADER_BUILD}/image.vert.spv ${SHADER_BUILD}/image.frag.spv
                ${SHADER_HEADER}
        DEPENDS ${SHADER_BUILD}/rect.vert.spv ${SHADER_BUILD}/rect.frag.spv
                ${SHADER_BUILD}/text.vert.spv ${SHADER_BUILD}/text.frag.spv
                ${SHADER_BUILD}/image.vert.spv ${SHADER_BUILD}/image.frag.spv
                ${EMBED_SCRIPT}
        VERBATIM COMMENT "Embedding SPIR-V → ShaderSpirv.h"
    )
    add_custom_target(genesis_shaders DEPENDS ${SHADER_HEADER})
    message(STATUS "Genesis shaders: auto-compile enabled (${GLSLC_EXECUTABLE})")
else()
    add_custom_target(genesis_shaders)
    message(STATUS "Genesis shaders: using pre-built ShaderSpirv.h")
endif()

# --- Dependencies ---
if(NOT WIN32)
    find_package(PkgConfig REQUIRED)
    if(UNIX AND NOT APPLE)
        pkg_check_modules(XCB         REQUIRED xcb)
        pkg_check_modules(XCB_KEYSYMS REQUIRED xcb-keysyms)
        pkg_check_modules(DBUS        REQUIRED dbus-1)
        pkg_check_modules(ATSPI       QUIET    atspi-2)
    endif()
endif()

# --- stb_truetype include path ---
set(STB_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/third_party)

# --- Core Library (header-only interface) ---
add_library(genesis_core INTERFACE)
target_include_directories(genesis_core INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${STB_INCLUDE_DIR}>
    $<INSTALL_INTERFACE:include>
)

# --- Optional modules (Qt-style opt-in: link the module, get the feature) ---

# genesis_db — SQLite3 wrapper (Database.h)
add_library(genesis_db INTERFACE)
target_link_libraries(genesis_db INTERFACE genesis_core)
find_package(SQLite3 QUIET)
if(SQLite3_FOUND)
    target_link_libraries(genesis_db INTERFACE SQLite::SQLite3)
else()
    find_library(SQLITE3_LIB sqlite3)
    if(SQLITE3_LIB)
        target_link_libraries(genesis_db INTERFACE ${SQLITE3_LIB})
    endif()
endif()

# genesis_io — serial port (SerialPort.h); POSIX/Win32 deps come from the OS
add_library(genesis_io INTERFACE)
target_link_libraries(genesis_io INTERFACE genesis_core)

# genesis_config — Settings + Json (no extra deps)
add_library(genesis_config INTERFACE)
target_link_libraries(genesis_config INTERFACE genesis_core)

# genesis_concurrent — WorkerThread (no extra deps beyond std::thread)
add_library(genesis_concurrent INTERFACE)
target_link_libraries(genesis_concurrent INTERFACE genesis_core)

# genesis_model — TableModel, TreeModel, SortFilterModel, ModelBinding
add_library(genesis_model INTERFACE)
target_link_libraries(genesis_model INTERFACE genesis_core)

# --- Platform Implementation ---
if(WIN32)
    # When cross-compiling, FindVulkan may fail or find native Linux libs.
    # Provide manual targets or standard system libraries.
    find_package(Vulkan QUIET)
    if(NOT Vulkan_FOUND)
        # Setup mock/header-only Vulkan dependencies or manual library mapping
        add_library(Vulkan::Vulkan INTERFACE IMPORTED)
        # MinGW needs /usr/include to locate vulkan/vulkan.h
        target_include_directories(Vulkan::Vulkan INTERFACE ${CMAKE_SOURCE_DIR}/include)
        # Use our generated import library or just link dynamically
        target_link_libraries(Vulkan::Vulkan INTERFACE ${CMAKE_SOURCE_DIR}/libvulkan-1.a)
    endif()
else()
    find_package(Vulkan REQUIRED)
endif()
add_library(genesis_platform STATIC)
target_link_libraries(genesis_platform PUBLIC genesis_core Vulkan::Vulkan)
add_dependencies(genesis_platform genesis_shaders)

if(UNIX AND NOT APPLE)
    target_sources(genesis_platform PRIVATE
        src/platforms/linux/LinuxPlatformWindow.cpp
        src/graphics/VulkanGpuHal.cpp
        src/graphics/FontEngineImpl.cpp
        src/accessibility/AtSpiServer.cpp
    )
    target_include_directories(genesis_platform PRIVATE
        ${XCB_INCLUDE_DIRS}
        ${XCB_KEYSYMS_INCLUDE_DIRS}
        ${DBUS_INCLUDE_DIRS}
        ${STB_INCLUDE_DIR}
    )
    target_link_libraries(genesis_platform PRIVATE
        ${XCB_LIBRARIES}
        ${XCB_KEYSYMS_LIBRARIES}
        ${DBUS_LIBRARIES}
        rt          # POSIX shared memory (AI control bus)
    )
    if(ATSPI_FOUND)
        target_include_directories(genesis_platform PRIVATE ${ATSPI_INCLUDE_DIRS})
        target_link_libraries(genesis_platform PRIVATE ${ATSPI_LIBRARIES})
        target_compile_definitions(genesis_platform PRIVATE GENESIS_ATSPI=1)
    endif()
elseif(WIN32)
    target_sources(genesis_platform PRIVATE
        src/platforms/windows/WindowsPlatformWindow.cpp
        src/graphics/VulkanGpuHal.cpp
        src/graphics/FontEngineImpl.cpp
    )
    target_include_directories(genesis_platform PRIVATE
        ${STB_INCLUDE_DIR}
    )
    # Windows native winsock, gdi, etc.
    target_link_libraries(genesis_platform PRIVATE
        ws2_32
        gdi32
        user32
        shell32
    )
    # Static link runtime libraries for standalone Win32 binaries
    add_link_options(-static -static-libgcc -static-libstdc++)
endif()

# --- Platform Host ---
add_executable(GenesisPlatformHost src/platforms/GenesisPlatformHost.cpp)
target_link_libraries(GenesisPlatformHost PRIVATE genesis_platform)

# --- Examples ---
if(GENESIS_BUILD_EXAMPLES)
    add_executable(genesis_performance_panel examples/performance_panel/main.cpp)
    target_link_libraries(genesis_performance_panel PRIVATE genesis_platform)

    add_executable(genesis_controls_catalog examples/controls_catalog/main.cpp)
    target_link_libraries(genesis_controls_catalog PRIVATE genesis_platform)
    # Copy translation files next to the binary
    add_custom_command(TARGET genesis_controls_catalog POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${CMAKE_SOURCE_DIR}/translations
                $<TARGET_FILE_DIR:genesis_controls_catalog>/translations
        COMMENT "Copying translations"
    )

    add_executable(genesis_dock_window_styles examples/dock_window_styles/main.cpp)
    target_link_libraries(genesis_dock_window_styles PRIVATE genesis_platform)

    add_executable(genesis_empty_window examples/empty_window/main.cpp)
    target_link_libraries(genesis_empty_window PRIVATE genesis_platform)
endif()

# --- AI tooling: external semantic probe + agent (read & drive a running app) ---
add_executable(genesis_ai_probe tools/ai_probe.cpp)
target_link_libraries(genesis_ai_probe PRIVATE genesis_core)
add_executable(genesis_ai_agent tools/ai_agent.cpp)
target_link_libraries(genesis_ai_agent PRIVATE genesis_core)
if(UNIX AND NOT APPLE)
    target_link_libraries(genesis_ai_probe PRIVATE rt pthread)
    target_link_libraries(genesis_ai_agent PRIVATE rt pthread)
endif()

# --- Tests ---
if(GENESIS_BUILD_TESTS)
    enable_testing()
    function(add_genesis_test name src)
        add_executable(${name} ${src})
        target_link_libraries(${name} PRIVATE genesis_platform)
        add_test(NAME ${name} COMMAND ${name})
    endfunction()

    add_genesis_test(test_signal           src/tests/test_signal.cpp)
    add_genesis_test(test_application      src/tests/test_application.cpp)
    add_genesis_test(test_scenegraph       src/tests/test_scenegraph.cpp)
    add_genesis_test(test_primitives       src/tests/test_primitives.cpp)
    add_genesis_test(test_gpuhal           src/tests/test_gpuhal.cpp)
    add_genesis_test(test_aicontrolbus     src/tests/test_aicontrolbus.cpp)
    add_genesis_test(test_widgets          src/tests/test_widgets.cpp)
    add_genesis_test(test_fontengine       src/tests/test_fontengine.cpp)
    add_genesis_test(test_style            src/tests/test_style.cpp)
    add_genesis_test(test_invalidation     src/tests/test_invalidation.cpp)
    add_genesis_test(test_assets           src/tests/test_assets.cpp)
    add_genesis_test(test_window           src/tests/test_window.cpp)
    add_genesis_test(test_components       src/tests/test_components.cpp)
    add_genesis_test(test_controls_foundation src/tests/test_controls_foundation.cpp)
    add_genesis_test(test_dock_options     src/tests/test_dock_options.cpp)
    add_genesis_test(test_json             src/tests/test_json.cpp)
    add_genesis_test(test_settings         src/tests/test_settings.cpp)
    add_genesis_test(test_databus          src/tests/test_databus.cpp)
    add_genesis_test(test_dispatcher       src/tests/test_dispatcher.cpp)
    add_genesis_test(test_dragdrop         src/tests/test_dragdrop.cpp)
    add_genesis_test(test_canvas           src/tests/test_canvas.cpp)
    add_genesis_test(test_database         src/tests/test_database.cpp)
    target_link_libraries(test_database PRIVATE genesis_db)
    add_genesis_test(test_serial           src/tests/test_serial.cpp)
    add_genesis_test(test_dialog           src/tests/test_dialog.cpp)
    add_genesis_test(test_aipanel          src/tests/test_aipanel.cpp)
    add_genesis_test(test_models           src/tests/test_models.cpp)
    add_genesis_test(test_new_features     src/tests/test_new_features.cpp)
endif()
