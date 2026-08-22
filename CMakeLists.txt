cmake_minimum_required(VERSION 3.25)

project(index VERSION 2.0.0 LANGUAGES CXX)
include(GNUInstallDirs)

set(WARGAME_VERSION "${PROJECT_VERSION}" CACHE STRING
    "Version embedded in desktop and server release artifacts")
set(WARGAME_SOURCE_DIGEST "dev" CACHE STRING
    "Source digest embedded in desktop and server release artifacts")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(WARGAME_BUILD_SERVER "Build the authoritative game server" ON)
option(WARGAME_BUILD_VMF_TOOLS
       "Build the design/EncoderDecoder VMF CLI tools when tinyxml2 is available" ON)
option(WARGAME_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(WARGAME_ENABLE_FASTDDS "Enable Fast DDS data-plane integration when the SDK is installed" OFF)

find_package(Qt6 6.10 REQUIRED COMPONENTS Quick QuickControls2 Network WebSockets)
find_package(Qt6Keychain REQUIRED)

if(WARGAME_ENABLE_FASTDDS)
    set(WARGAME_FASTDDS_TARGET "")
    set(WARGAME_FASTDDS_LEGACY_API OFF)
    find_package(fastdds QUIET CONFIG)
    if(fastdds_FOUND)
        if(TARGET fastdds::fastdds)
            set(WARGAME_FASTDDS_TARGET fastdds::fastdds)
        elseif(TARGET fastdds)
            set(WARGAME_FASTDDS_TARGET fastdds)
        endif()
    endif()
    # Ubuntu 24.04 ships the same API under the legacy package/config name.
    if(NOT WARGAME_FASTDDS_TARGET)
        find_package(fastrtps QUIET CONFIG)
        if(fastrtps_FOUND AND TARGET fastrtps)
            set(WARGAME_FASTDDS_TARGET fastrtps)
            set(WARGAME_FASTDDS_LEGACY_API ON)
        endif()
    endif()
    find_program(WARGAME_FASTDDSGEN_EXECUTABLE NAMES fastddsgen)
    if(WARGAME_FASTDDS_TARGET AND WARGAME_FASTDDSGEN_EXECUTABLE)
        set(WARGAME_HAS_FASTDDS ON)
    elseif(WARGAME_FASTDDS_TARGET)
        message(WARNING "Fast DDS SDK found but fastddsgen is missing; DDS integration is disabled")
    endif()
endif()

qt_standard_project_setup(REQUIRES 6.10)

# The standalone EncoderDecoder project is also part of the release contract.
# Keep it optional for hosts that only need the Qt client/server, but make the
# disabled state explicit instead of silently shipping dictionary-only assets.
set(WARGAME_VMF_TOOLS_AVAILABLE OFF)
if(WARGAME_BUILD_VMF_TOOLS)
    find_library(WARGAME_TINYXML2_LIBRARY NAMES tinyxml2)
    if(WARGAME_TINYXML2_LIBRARY)
        set(WARGAME_VMF_TOOLS_BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/vmf-tools")
        add_subdirectory(
            "${CMAKE_CURRENT_SOURCE_DIR}/design/EncoderDecoder"
            "${WARGAME_VMF_TOOLS_BINARY_DIR}")
        set(WARGAME_VMF_TOOLS_AVAILABLE ON)
        message(STATUS "VMF EncoderDecoder CLI: enabled")
    else()
        message(WARNING
            "VMF EncoderDecoder CLI: disabled (tinyxml2 not found); "
            "install libtinyxml2-dev or set WARGAME_BUILD_VMF_TOOLS=OFF")
    endif()
else()
    message(STATUS "VMF EncoderDecoder CLI: disabled by WARGAME_BUILD_VMF_TOOLS=OFF")
endif()

if(WARGAME_VMF_TOOLS_AVAILABLE)
    find_program(WARGAME_BASH_EXECUTABLE NAMES bash REQUIRED)
    add_custom_target(vmf_design_regression
        COMMAND "${WARGAME_BASH_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/vmf-design-regression.sh"
                --source-dir "${CMAKE_CURRENT_SOURCE_DIR}"
                --build-dir "${WARGAME_VMF_TOOLS_BINARY_DIR}"
                --out-dir "${CMAKE_CURRENT_BINARY_DIR}/vmf-design-regression"
        DEPENDS vmf_encode vmf_decode vmf_validate xml_compare
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "Running design/EncoderDecoder VMF regression pipeline"
        VERBATIM)
endif()

include(cmake/WargameTargets.cmake)
wargame_add_vmf_library(wargame_vmf ${CMAKE_CURRENT_SOURCE_DIR})
wargame_add_domain_library(wargame_domain ${CMAKE_CURRENT_SOURCE_DIR})
wargame_add_protocol_library(wargame_protocol ${CMAKE_CURRENT_SOURCE_DIR})
if(WARGAME_HAS_FASTDDS)
    set(WARGAME_DDS_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/dds")
    set(WARGAME_DDS_GENERATED_SOURCES
        "${WARGAME_DDS_GENERATED_DIR}/WargameEnvelope.cxx"
        "${WARGAME_DDS_GENERATED_DIR}/WargameEnvelopePubSubTypes.cxx"
        "${WARGAME_DDS_GENERATED_DIR}/WargameEnvelopeTypeObjectSupport.cxx")
    add_custom_command(
        OUTPUT ${WARGAME_DDS_GENERATED_SOURCES}
        COMMAND ${CMAKE_COMMAND} -E make_directory "${WARGAME_DDS_GENERATED_DIR}"
        COMMAND ${CMAKE_COMMAND}
                "-DWARGAME_FASTDDSGEN_EXECUTABLE=${WARGAME_FASTDDSGEN_EXECUTABLE}"
                "-DWARGAME_DDS_GENERATED_DIR=${WARGAME_DDS_GENERATED_DIR}"
                "-DWARGAME_DDS_IDL=${CMAKE_CURRENT_SOURCE_DIR}/src/protocol/dds/WargameEnvelope.idl"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateFastDds.cmake"
        DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/src/protocol/dds/WargameEnvelope.idl"
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateFastDds.cmake"
        COMMENT "Generating Fast DDS WargameEnvelope type"
        VERBATIM)
    add_custom_target(wargame_fastdds_generated DEPENDS ${WARGAME_DDS_GENERATED_SOURCES})
    add_dependencies(wargame_protocol wargame_fastdds_generated)
    target_sources(wargame_protocol PRIVATE ${WARGAME_DDS_GENERATED_SOURCES})
    target_include_directories(wargame_protocol PUBLIC "${WARGAME_DDS_GENERATED_DIR}")
    target_compile_definitions(wargame_protocol PUBLIC WARGAME_HAS_FASTDDS=1)
    if(WARGAME_FASTDDS_LEGACY_API)
        target_compile_definitions(wargame_protocol PUBLIC WARGAME_FASTDDS_LEGACY_API=1)
    endif()
    target_link_libraries(wargame_protocol PUBLIC ${WARGAME_FASTDDS_TARGET})
endif()
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
    src/network/FastDdsTransport.h
    src/network/FastDdsTransport.cpp
)
target_link_libraries(wargame_client PUBLIC
    wargame_domain wargame_protocol wargame_vmf Qt6::Quick Qt6::Network Qt6::WebSockets Qt6Keychain::Qt6Keychain)
if(WARGAME_HAS_FASTDDS)
    target_compile_definitions(wargame_client PUBLIC WARGAME_HAS_FASTDDS=1)
    target_link_libraries(wargame_client PUBLIC ${WARGAME_FASTDDS_TARGET})
endif()
target_include_directories(wargame_client PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
set_target_properties(wargame_client PROPERTIES AUTOMOC ON)
wargame_enable_sanitizers(wargame_client)

# Compatibility name for downstream code that still links the old target.
add_library(corelib ALIAS wargame_client)

qt_add_executable(appindex
    main.cpp
)
target_link_libraries(appindex PRIVATE corelib Qt6::Quick Qt6::QuickControls2)
target_compile_definitions(appindex PRIVATE
    WARGAME_VERSION="${WARGAME_VERSION}"
    WARGAME_SOURCE_DIGEST="${WARGAME_SOURCE_DIGEST}")
wargame_enable_sanitizers(appindex)

set_source_files_properties(qml/AppContext.qml PROPERTIES
    QT_QML_SINGLETON_TYPE TRUE
)

qt_add_qml_module(appindex
    URI index
    QML_FILES
        Main.qml
        qml/AppContext.qml
        qml/SimulationRoot.qml
        qml/views/CommandPostView.qml
        qml/views/DirectorView.qml
        qml/views/OnlineOperationsView.qml
        qml/views/RoomAdminView.qml
        qml/views/ScenarioEditorView.qml
        qml/views/RosterEditorDialog.qml
        qml/components/MapCanvas.qml
        qml/components/MessageLog.qml
        qml/components/EventDialog.qml
        qml/components/UnitPanel.qml
        qml/components/AbilityActionButton.qml
        qml/components/UnitDetailsDialog.qml
        qml/components/SectionTitle.qml
        qml/components/TonalButton.qml
        qml/components/GhostButton.qml
        qml/components/Icon.qml
        qml/components/UnitEditDialog.qml
        qml/components/RoutePlannerDialog.qml
        qml/components/Minimap.qml
        qml/components/SettingsPanel.qml
        qml/components/SettingsSection.qml
        qml/components/SettingsToggleRow.qml
        qml/components/ShortcutEditor.qml
        qml/components/LocalSettingsPanel.qml
        qml/components/OnlineSettingsPanel.qml
        qml/components/OnlineIntelWorkspace.qml
        qml/components/IntelTextField.qml
        qml/components/IntelComboBox.qml
        qml/components/SessionDialog.qml
        qml/components/ChatPanel.qml
        qml/components/BattleReportDialog.qml
        qml/components/CommandPanel.qml
        qml/components/GuidedStrikeWorkflowPanel.qml
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
file(GLOB WARGAME_MAP_CANDIDATE_DIRS CONFIGURE_DEPENDS
    LIST_DIRECTORIES true "${WARGAME_MAP_SOURCE_DIR}/*")
set(WARGAME_MAP_ZOOM_DIRS)
set(WARGAME_MAP_TILES)
foreach(map_candidate_dir IN LISTS WARGAME_MAP_CANDIDATE_DIRS)
    if(IS_DIRECTORY "${map_candidate_dir}")
        get_filename_component(map_zoom_name "${map_candidate_dir}" NAME)
        if(map_zoom_name MATCHES "^[0-9]+$")
            list(APPEND WARGAME_MAP_ZOOM_DIRS "${map_candidate_dir}")
            file(GLOB_RECURSE map_zoom_tiles CONFIGURE_DEPENDS
                "${map_candidate_dir}/*.png")
            list(APPEND WARGAME_MAP_TILES ${map_zoom_tiles})
        endif()
    endif()
endforeach()
if(NOT WARGAME_MAP_ZOOM_DIRS OR NOT WARGAME_MAP_TILES)
    message(FATAL_ERROR "No GIS tiles found under numeric zoom directories in ${WARGAME_MAP_SOURCE_DIR}")
endif()
list(SORT WARGAME_MAP_ZOOM_DIRS)
list(SORT WARGAME_MAP_TILES)
set(WARGAME_MAP_STAGE_COMMANDS)
foreach(map_zoom_dir IN LISTS WARGAME_MAP_ZOOM_DIRS)
    get_filename_component(map_zoom_name "${map_zoom_dir}" NAME)
    list(APPEND WARGAME_MAP_STAGE_COMMANDS
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${map_zoom_dir}" "${WARGAME_MAP_STAGE_DIR}/${map_zoom_name}")
endforeach()
foreach(map_metadata_file IN LISTS WARGAME_MAP_METADATA)
    if(NOT EXISTS "${map_metadata_file}")
        message(FATAL_ERROR "Required GIS metadata is missing: ${map_metadata_file}")
    endif()
endforeach()

# Track additions/removals as well as file content changes, so a stale tile
# cannot survive in the staged directory after the source set changes.
string(REPLACE ";" "\n" WARGAME_MAP_FILE_LIST_CONTENT "${WARGAME_MAP_TILES}")
set(WARGAME_MAP_FILE_LIST "${CMAKE_CURRENT_BINARY_DIR}/wargame-map-files.txt")
file(GENERATE OUTPUT "${WARGAME_MAP_FILE_LIST}"
    CONTENT "${WARGAME_MAP_FILE_LIST_CONTENT}\n")

add_custom_command(
    OUTPUT "${WARGAME_MAP_STAGE_DIR}/.stage-stamp"
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${WARGAME_MAP_STAGE_DIR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${WARGAME_MAP_STAGE_DIR}"
    ${WARGAME_MAP_STAGE_COMMANDS}
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
foreach(map_zoom_dir IN LISTS WARGAME_MAP_ZOOM_DIRS)
    install(DIRECTORY "${map_zoom_dir}"
        DESTINATION "${CMAKE_INSTALL_BINDIR}/map")
endforeach()
install(FILES ${WARGAME_MAP_METADATA}
    DESTINATION "${CMAKE_INSTALL_BINDIR}/map")

# VMF dictionaries are runtime inputs, not generated build output. Keep the
# profile beside the installed executable so it can load without the source
# checkout.
set(WARGAME_VMF_INSTALL_DIR "${CMAKE_INSTALL_BINDIR}/design/EncoderDecoder")
install(FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/design/EncoderDecoder/dic.xml"
    "${CMAKE_CURRENT_SOURCE_DIR}/design/EncoderDecoder/dic_content.xml"
    "${CMAKE_CURRENT_SOURCE_DIR}/design/EncoderDecoder/message_catalog.json"
    DESTINATION "${WARGAME_VMF_INSTALL_DIR}")
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/design/EncoderDecoder/msgStruct"
    DESTINATION "${WARGAME_VMF_INSTALL_DIR}"
    FILES_MATCHING PATTERN "*.xml")
if(WARGAME_VMF_TOOLS_AVAILABLE)
    install(TARGETS vmf_encode vmf_decode vmf_validate xml_compare
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# Authoritative headless server. It can also be built standalone from server/.
if(WARGAME_BUILD_SERVER)
    add_subdirectory(server)
endif()

include(CTest)
if(BUILD_TESTING)
    include(FetchContent)
    set(INSTALL_GTEST OFF CACHE BOOL "Do not install embedded GoogleTest" FORCE)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.17.0
        GIT_SHALLOW    TRUE
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
        tests/test_combat_resolver.cpp
        tests/test_maptiles.cpp
        tests/test_transport.cpp
        tests/test_locksync.cpp
        tests/test_snapshot.cpp
        tests/test_controller.cpp
        tests/test_protocol.cpp
        tests/test_command_result.cpp
        tests/test_checkpoint.cpp
        tests/test_p3.cpp
        tests/test_client_state_store.cpp
        tests/test_fastdds_transport.cpp
        tests/test_vmf_codec.cpp
        tests/test_vmf_catalog.cpp
        tests/test_vmf_workflow.cpp
    )
    target_link_libraries(wargame_tests PRIVATE
        corelib wargame_protocol wargame_vmf GTest::gtest GTest::gtest_main)
    target_include_directories(wargame_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(wargame_tests PRIVATE
        WARGAME_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    wargame_enable_sanitizers(wargame_tests)

    add_executable(fastdds_integration_tests tests/test_fastdds_integration.cpp)
    target_link_libraries(fastdds_integration_tests PRIVATE
        wargame_client wargame_protocol GTest::gtest GTest::gtest_main)
    if(WARGAME_HAS_FASTDDS)
        target_compile_definitions(fastdds_integration_tests PRIVATE WARGAME_HAS_FASTDDS=1)
        target_link_libraries(fastdds_integration_tests PRIVATE ${WARGAME_FASTDDS_TARGET})
    endif()
    target_include_directories(fastdds_integration_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    wargame_enable_sanitizers(fastdds_integration_tests)
    add_test(NAME FastDDS.Integration COMMAND fastdds_integration_tests)

    if(TARGET wargame_serverlib)
        target_sources(wargame_tests PRIVATE
            tests/test_room_persistence.cpp
            tests/test_intel_ledger.cpp
            tests/test_state_projector.cpp
            tests/test_rules_ai.cpp
            tests/test_ollama_conversation_store.cpp
            tests/test_strict_vmf_task.cpp
        )
        target_link_libraries(wargame_tests PRIVATE wargame_serverlib)

        add_executable(authoritative_room_tests
            tests/test_authoritative_room.cpp
        )
        target_link_libraries(authoritative_room_tests PRIVATE
            wargame_serverlib GTest::gtest GTest::gtest_main)
        target_include_directories(authoritative_room_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        wargame_enable_sanitizers(authoritative_room_tests)
        add_test(NAME GameServer.AuthoritativeRoom COMMAND authoritative_room_tests)

        add_executable(observer_server_tests
            tests/test_observer_server.cpp
        )
        target_link_libraries(observer_server_tests PRIVATE
            wargame_serverlib GTest::gtest)
        target_include_directories(observer_server_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        wargame_enable_sanitizers(observer_server_tests)
        add_test(NAME GameServer.Observer COMMAND observer_server_tests)

        add_executable(ollama_provider_tests
            tests/test_ollama_provider.cpp
            tests/test_ollama_dynamic.cpp
        )
        target_link_libraries(ollama_provider_tests PRIVATE
            wargame_serverlib GTest::gtest)
        target_include_directories(ollama_provider_tests PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/server/game)
        wargame_enable_sanitizers(ollama_provider_tests)
        add_test(NAME GameServer.OllamaProvider COMMAND ollama_provider_tests)
    endif()

    include(GoogleTest)
    gtest_discover_tests(wargame_tests)
endif()
