cmake_minimum_required(VERSION 3.16)

project(index VERSION 0.1 LANGUAGES CXX)
include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(WARGAME_BUILD_SERVER "Build the authoritative game server" ON)
option(WARGAME_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

find_package(Qt6 6.10 REQUIRED COMPONENTS Quick QuickControls2 Network WebSockets)

qt_standard_project_setup(REQUIRES 6.10)

include(cmake/WargameTargets.cmake)
wargame_add_domain_library(wargame_domain ${CMAKE_CURRENT_SOURCE_DIR})
wargame_add_protocol_library(wargame_protocol ${CMAKE_CURRENT_SOURCE_DIR})
wargame_enable_sanitizers(wargame_domain)
wargame_enable_sanitizers(wargame_protocol)

# Client-only code. The authoritative server links wargame_domain and
# wargame_protocol directly and therefore has no Qt Quick/QML dependency.
add_library(wargame_client STATIC
    src/core/TileCacheLocator.h
    src/core/TileCacheLocator.cpp
    src/core/TileImageProvider.h
    src/core/TileImageProvider.cpp
    src/view/SimulationController.h
    src/view/SimulationController.cpp
    src/view/ScenarioEditor.h
    src/view/ScenarioEditor.cpp
    src/view/MapTileRenderer.h
    src/view/MapTileRenderer.cpp
    src/network/NetworkClient.h
    src/network/NetworkClient.cpp
    src/network/ClientStateStore.h
    src/network/ClientStateStore.cpp
)
target_link_libraries(wargame_client PUBLIC
    wargame_domain wargame_protocol Qt6::Quick Qt6::Network Qt6::WebSockets)
target_include_directories(wargame_client PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
set_target_properties(wargame_client PROPERTIES AUTOMOC ON)
wargame_enable_sanitizers(wargame_client)

# Compatibility name for downstream code that still links the old target.
add_library(corelib ALIAS wargame_client)

qt_add_executable(appindex
    main.cpp
)
target_link_libraries(appindex PRIVATE corelib Qt6::Quick Qt6::QuickControls2)
wargame_enable_sanitizers(appindex)

qt_add_qml_module(appindex
    URI index
    QML_FILES
        Main.qml
        qml/SimulationRoot.qml
        qml/views/CommandPostView.qml
        qml/views/DirectorView.qml
        qml/views/ScenarioEditorView.qml
        qml/views/RosterEditorDialog.qml
        qml/components/MapCanvas.qml
        qml/components/MessageLog.qml
        qml/components/EventDialog.qml
        qml/components/UnitPanel.qml
        qml/components/SectionTitle.qml
        qml/components/TonalButton.qml
        qml/components/GhostButton.qml
        qml/components/Icon.qml
        qml/components/UnitEditDialog.qml
        qml/components/RoutePlannerDialog.qml
        qml/components/Minimap.qml
        qml/components/SettingsPanel.qml
        qml/components/SessionDialog.qml
        qml/components/ChatPanel.qml
)

set_target_properties(appindex PROPERTIES
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
    MACOSX_BUNDLE TRUE
    WIN32_EXECUTABLE TRUE
)

# Stage the runtime GIS subset next to the executable.  The source tree's
# map/ directory is authoritative; generated imagery and authoring tools are
# intentionally excluded from runtime/install output.
set(WARGAME_MAP_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/map")
set(WARGAME_MAP_STAGE_DIR "${CMAKE_CURRENT_BINARY_DIR}/map")
set(WARGAME_MAP_METADATA
    "${WARGAME_MAP_SOURCE_DIR}/metadata.json"
    "${WARGAME_MAP_SOURCE_DIR}/tilejson.json"
)
file(GLOB_RECURSE WARGAME_MAP_TILES CONFIGURE_DEPENDS
    "${WARGAME_MAP_SOURCE_DIR}/12/*.png"
)
if(NOT WARGAME_MAP_TILES)
    message(FATAL_ERROR "No GIS tiles found under ${WARGAME_MAP_SOURCE_DIR}/12")
endif()
foreach(map_metadata_file IN LISTS WARGAME_MAP_METADATA)
    if(NOT EXISTS "${map_metadata_file}")
        message(FATAL_ERROR "Required GIS metadata is missing: ${map_metadata_file}")
    endif()
endforeach()

# Track additions/removals as well as file content changes, so a stale tile
# cannot survive in the staged directory after the source set changes.
list(SORT WARGAME_MAP_TILES)
string(REPLACE ";" "\n" WARGAME_MAP_FILE_LIST_CONTENT "${WARGAME_MAP_TILES}")
set(WARGAME_MAP_FILE_LIST "${CMAKE_CURRENT_BINARY_DIR}/wargame-map-files.txt")
file(GENERATE OUTPUT "${WARGAME_MAP_FILE_LIST}"
    CONTENT "${WARGAME_MAP_FILE_LIST_CONTENT}\n")

add_custom_command(
    OUTPUT "${WARGAME_MAP_STAGE_DIR}/.stage-stamp"
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${WARGAME_MAP_STAGE_DIR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${WARGAME_MAP_STAGE_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${WARGAME_MAP_SOURCE_DIR}/12" "${WARGAME_MAP_STAGE_DIR}/12"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${WARGAME_MAP_METADATA} "${WARGAME_MAP_STAGE_DIR}"
    COMMAND ${CMAKE_COMMAND} -E touch "${WARGAME_MAP_STAGE_DIR}/.stage-stamp"
    DEPENDS ${WARGAME_MAP_TILES} ${WARGAME_MAP_METADATA} "${WARGAME_MAP_FILE_LIST}"
    COMMENT "Staging GIS map data"
    VERBATIM
)
add_custom_target(stage_wargame_map ALL
    DEPENDS "${WARGAME_MAP_STAGE_DIR}/.stage-stamp")
add_dependencies(appindex stage_wargame_map)

install(TARGETS appindex
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(DIRECTORY "${WARGAME_MAP_SOURCE_DIR}/12"
    DESTINATION "${CMAKE_INSTALL_BINDIR}/map")
install(FILES ${WARGAME_MAP_METADATA}
    DESTINATION "${CMAKE_INSTALL_BINDIR}/map")

# Authoritative headless server. It can also be built standalone from server/.
if(WARGAME_BUILD_SERVER)
    add_subdirectory(server)
endif()

# ── Unit tests ──────────────────────────────────────────────────
include(CTest)
if(BUILD_TESTING)
include(FetchContent)
set(INSTALL_GTEST OFF CACHE BOOL "Do not install embedded GoogleTest" FORCE)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.17.0
)
FetchContent_MakeAvailable(googletest)

enable_testing()

add_executable(wargame_tests
    tests/test_geo.cpp
    tests/test_unitfsm.cpp
    tests/test_messagebus.cpp
    tests/test_scenario.cpp
    tests/test_engine.cpp
    tests/test_phase1_fixes.cpp
    tests/test_attack_power.cpp
    tests/test_maptiles.cpp
    tests/test_transport.cpp
    tests/test_locksync.cpp
    tests/test_snapshot.cpp
    tests/test_controller.cpp
    tests/test_protocol.cpp
    tests/test_command_result.cpp
    tests/test_checkpoint.cpp
    tests/test_client_state_store.cpp
)
target_link_libraries(wargame_tests PRIVATE
    corelib wargame_protocol GTest::gtest GTest::gtest_main)
target_include_directories(wargame_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
wargame_enable_sanitizers(wargame_tests)

if(TARGET wargame_serverlib)
    target_sources(wargame_tests PRIVATE
        tests/test_room_persistence.cpp
        tests/test_state_projector.cpp
    )
    target_link_libraries(wargame_tests PRIVATE wargame_serverlib)
endif()

include(GoogleTest)
gtest_discover_tests(wargame_tests)
endif()
