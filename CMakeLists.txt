cmake_minimum_required(VERSION 3.16)
project(JFramework VERSION 0.1.0 LANGUAGES CXX)

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
    set(SHADER_HEADER ${CMAKE_SOURCE_DIR}/include/j/graphics/ShaderSpirv.h)
    set(EMBED_SCRIPT  ${CMAKE_SOURCE_DIR}/cmake/embed_spv.py)
    file(MAKE_DIRECTORY ${SHADER_BUILD})

    foreach(SH rect.vert rect.frag text.vert text.frag image.vert image.frag vector.vert vector.frag)
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
                ${SHADER_BUILD}/vector.vert.spv ${SHADER_BUILD}/vector.frag.spv
                ${SHADER_HEADER}
        DEPENDS ${SHADER_BUILD}/rect.vert.spv ${SHADER_BUILD}/rect.frag.spv
                ${SHADER_BUILD}/text.vert.spv ${SHADER_BUILD}/text.frag.spv
                ${SHADER_BUILD}/image.vert.spv ${SHADER_BUILD}/image.frag.spv
                ${SHADER_BUILD}/vector.vert.spv ${SHADER_BUILD}/vector.frag.spv
                ${EMBED_SCRIPT}
        VERBATIM COMMENT "Embedding SPIR-V → ShaderSpirv.h"
    )
    add_custom_target(j_shaders DEPENDS ${SHADER_HEADER})
    message(STATUS "Genesis shaders: auto-compile enabled (${GLSLC_EXECUTABLE})")
else()
    add_custom_target(j_shaders)
    message(STATUS "Genesis shaders: using pre-built ShaderSpirv.h")
endif()

# --- Dependencies ---
if(NOT WIN32)
    find_package(PkgConfig REQUIRED)
    if(UNIX AND NOT APPLE)
        pkg_check_modules(XCB         REQUIRED xcb)
        pkg_check_modules(XCB_KEYSYMS REQUIRED xcb-keysyms)
        pkg_check_modules(XCB_SYNC    REQUIRED xcb-sync)
        pkg_check_modules(DBUS        REQUIRED dbus-1)
        pkg_check_modules(ATSPI       QUIET    atspi-2)
    endif()
endif()

# --- stb_truetype include path ---
set(STB_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/third_party)

# --- Core Library (header-only interface) ---
add_library(j_core INTERFACE)
target_include_directories(j_core INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${STB_INCLUDE_DIR}>
    $<INSTALL_INTERFACE:include>
)

# --- Optional modules (Qt-style opt-in: link the module, get the feature) ---

# j_db — SQLite3 wrapper (Database.h)
add_library(j_db INTERFACE)
target_link_libraries(j_db INTERFACE j_core)
find_package(SQLite3 QUIET)
if(SQLite3_FOUND)
    target_link_libraries(j_db INTERFACE SQLite::SQLite3)
else()
    find_library(SQLITE3_LIB sqlite3)
    if(SQLITE3_LIB)
        target_link_libraries(j_db INTERFACE ${SQLITE3_LIB})
    endif()
endif()

# j_io — serial port (SerialPort.h); POSIX/Win32 deps come from the OS
add_library(j_io INTERFACE)
target_link_libraries(j_io INTERFACE j_core)

# j_config — Settings + Json (no extra deps)
add_library(j_config INTERFACE)
target_link_libraries(j_config INTERFACE j_core)

# j_concurrent — WorkerThread (no extra deps beyond std::thread)
add_library(j_concurrent INTERFACE)
target_link_libraries(j_concurrent INTERFACE j_core)

# j_model — TableModel, TreeModel, SortFilterModel, ModelBinding
add_library(j_model INTERFACE)
target_link_libraries(j_model INTERFACE j_core)

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
add_library(j_platform STATIC)
target_link_libraries(j_platform PUBLIC j_core Vulkan::Vulkan)
add_dependencies(j_platform j_shaders)
# Native-window factory (jf::createPlatformWindow) — keeps app code free of xcb/win32.
target_sources(j_platform PRIVATE src/platforms/PlatformFactory.cpp)
# Serial backend (JSerialPort) — POSIX/Win32 impl behind the pimpl; header stays platform-free.
target_sources(j_platform PRIVATE src/io/SerialPort.cpp)

if(UNIX AND NOT APPLE)
    target_sources(j_platform PRIVATE
        src/platforms/linux/LinuxPlatformWindow.cpp
        src/graphics/VulkanGpuHal.cpp
        src/graphics/FontEngineImpl.cpp
        src/accessibility/AtSpiServer.cpp
    )
    target_include_directories(j_platform PRIVATE
        ${XCB_INCLUDE_DIRS}
        ${XCB_KEYSYMS_INCLUDE_DIRS}
        ${XCB_SYNC_INCLUDE_DIRS}
        ${DBUS_INCLUDE_DIRS}
        ${STB_INCLUDE_DIR}
    )
    target_link_libraries(j_platform PRIVATE
        ${XCB_LIBRARIES}
        ${XCB_KEYSYMS_LIBRARIES}
        ${XCB_SYNC_LIBRARIES}
        ${DBUS_LIBRARIES}
        rt          # POSIX shared memory (AI control bus)
    )
    if(ATSPI_FOUND)
        target_include_directories(j_platform PRIVATE ${ATSPI_INCLUDE_DIRS})
        target_link_libraries(j_platform PRIVATE ${ATSPI_LIBRARIES})
        target_compile_definitions(j_platform PRIVATE GENESIS_ATSPI=1)
    endif()
elseif(WIN32)
    target_sources(j_platform PRIVATE
        src/platforms/windows/WindowsPlatformWindow.cpp
        src/graphics/VulkanGpuHal.cpp
        src/graphics/FontEngineImpl.cpp
    )
    target_include_directories(j_platform PRIVATE
        ${STB_INCLUDE_DIR}
    )
    # Windows native winsock, gdi, etc.
    target_link_libraries(j_platform PRIVATE
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
target_link_libraries(GenesisPlatformHost PRIVATE j_platform)

# --- Examples ---
if(GENESIS_BUILD_EXAMPLES)
    add_executable(j_performance_panel examples/performance_panel/main.cpp)
    target_link_libraries(j_performance_panel PRIVATE j_platform)

    add_executable(j_controls_catalog examples/controls_catalog/main.cpp)
    target_link_libraries(j_controls_catalog PRIVATE j_platform)
    # Copy translation files next to the binary
    add_custom_command(TARGET j_controls_catalog POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${CMAKE_SOURCE_DIR}/translations
                $<TARGET_FILE_DIR:j_controls_catalog>/translations
        COMMENT "Copying translations"
    )

    add_executable(j_dock_window_styles examples/dock_window_styles/main.cpp)
    target_link_libraries(j_dock_window_styles PRIVATE j_platform)

    add_executable(j_empty_window examples/empty_window/main.cpp)
    target_link_libraries(j_empty_window PRIVATE j_platform)

    add_executable(j_vector_demo examples/vector_demo/main.cpp)
    target_link_libraries(j_vector_demo PRIVATE j_platform)

    add_executable(j_app_shell examples/app_shell/main.cpp)
    target_link_libraries(j_app_shell PRIVATE j_platform)

    add_executable(j_chart_demo examples/chart_demo/main.cpp)
    target_link_libraries(j_chart_demo PRIVATE j_platform)
endif()

# --- Tests ---
if(GENESIS_BUILD_TESTS)
    enable_testing()
    function(add_genesis_test name src)
        add_executable(${name} ${src})
        target_link_libraries(${name} PRIVATE j_platform)
        add_test(NAME ${name} COMMAND ${name})
    endfunction()

    add_genesis_test(test_signal           src/tests/test_signal.cpp)
    add_genesis_test(test_application      src/tests/test_application.cpp)
    add_genesis_test(test_scenegraph       src/tests/test_scenegraph.cpp)
    add_genesis_test(test_primitives       src/tests/test_primitives.cpp)
    add_genesis_test(test_gpuhal           src/tests/test_gpuhal.cpp)
    add_genesis_test(test_widgets          src/tests/test_widgets.cpp)
    add_genesis_test(test_fontengine       src/tests/test_fontengine.cpp)
    add_genesis_test(test_style            src/tests/test_style.cpp)
    add_genesis_test(test_invalidation     src/tests/test_invalidation.cpp)
    add_genesis_test(test_assets           src/tests/test_assets.cpp)
    add_genesis_test(test_window           src/tests/test_window.cpp)
    add_genesis_test(test_undostack        src/tests/test_undostack.cpp)
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
    target_link_libraries(test_database PRIVATE j_db)
    add_genesis_test(test_serial           src/tests/test_serial.cpp)
    add_genesis_test(test_dialog           src/tests/test_dialog.cpp)
    add_genesis_test(test_models           src/tests/test_models.cpp)
    add_genesis_test(test_new_features     src/tests/test_new_features.cpp)
    add_genesis_test(test_variant          src/tests/test_variant.cpp)
    add_genesis_test(test_variant_integration src/tests/test_variant_integration.cpp)
    add_genesis_test(test_vector            src/tests/test_vector.cpp)
    add_genesis_test(test_chart             src/tests/test_chart.cpp)
    add_genesis_test(test_reference         src/tests/test_reference.cpp)
endif()

# ============================================================================
# Install / export — makes Genesis consumable via find_package(Genesis CONFIG).
# A consumer (an app sitting on the framework) does:
#     find_package(Genesis 0.1 CONFIG REQUIRED)
#     target_link_libraries(myapp PRIVATE Genesis::platform)   # or Genesis::core
# and never touches the framework source. This is the one seam between Genesis
# and the apps on top of it; the framework knows nothing about those apps.
# ============================================================================
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# Public headers: the genesis/** tree + the bundled stb header FontEngine.h needs.
install(DIRECTORY include/j DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY ${STB_INCLUDE_DIR}/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        FILES_MATCHING PATTERN "stb_*.h")

# Clean exported names: Genesis::core / Genesis::platform (not Genesis::j_*).
set_target_properties(j_core       PROPERTIES EXPORT_NAME core)
set_target_properties(j_io         PROPERTIES EXPORT_NAME io)
set_target_properties(j_config     PROPERTIES EXPORT_NAME config)
set_target_properties(j_concurrent PROPERTIES EXPORT_NAME concurrent)
set_target_properties(j_model      PROPERTIES EXPORT_NAME model)
set_target_properties(j_platform   PROPERTIES EXPORT_NAME platform)

# Export the core + header-only modules + the static platform lib under Genesis::.
# j_db is intentionally omitted (optional SQLite module) — add it later with
# its own find_dependency(SQLite3) when a consumer actually needs the database.
install(TARGETS
            j_core j_io j_config j_concurrent
            j_model j_platform
        EXPORT JFrameworkTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})

install(EXPORT JFrameworkTargets
        FILE JFrameworkTargets.cmake
        NAMESPACE JFramework::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/JFramework)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/JFrameworkConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/JFrameworkConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/JFramework)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/JFrameworkConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

install(FILES
            ${CMAKE_CURRENT_BINARY_DIR}/JFrameworkConfig.cmake
            ${CMAKE_CURRENT_BINARY_DIR}/JFrameworkConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/JFramework)
