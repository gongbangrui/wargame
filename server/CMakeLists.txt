cmake_minimum_required(VERSION 3.25)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(wargame_server VERSION 1.0.0 LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    option(WARGAME_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
    option(WARGAME_ENABLE_FASTDDS "Enable Fast DDS data-plane integration when the SDK is installed" OFF)
    find_package(Qt6 6.4 REQUIRED COMPONENTS Core Network WebSockets)
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
    include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/WargameTargets.cmake)
    wargame_add_domain_library(wargame_domain ${CMAKE_CURRENT_SOURCE_DIR}/..)
    wargame_add_protocol_library(wargame_protocol ${CMAKE_CURRENT_SOURCE_DIR}/..)
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
                    "-DWARGAME_DDS_IDL=${CMAKE_CURRENT_SOURCE_DIR}/../src/protocol/dds/WargameEnvelope.idl"
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/GenerateFastDds.cmake"
            DEPENDS
                "${CMAKE_CURRENT_SOURCE_DIR}/../src/protocol/dds/WargameEnvelope.idl"
                "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/GenerateFastDds.cmake"
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
endif()

if(NOT DEFINED WARGAME_VERSION)
    set(WARGAME_VERSION "${PROJECT_VERSION}" CACHE STRING
        "Version embedded in server release artifacts")
endif()
if(NOT DEFINED WARGAME_SOURCE_DIGEST)
    set(WARGAME_SOURCE_DIGEST "dev" CACHE STRING
        "Source digest embedded in server release artifacts")
endif()

add_library(wargame_serverlib STATIC
    game/AuthoritativeRoom.h
    game/AuthoritativeRoom.cpp
    game/GameServer.h
    game/GameServer.cpp
    game/RoomPersistence.h
    game/RoomPersistence.cpp
    game/StateProjector.h
    game/StateProjector.cpp
    game/FastDdsNode.h
    game/FastDdsNode.cpp
)
target_include_directories(wargame_serverlib PUBLIC ../src game)
target_link_libraries(wargame_serverlib PUBLIC
    wargame_domain wargame_protocol Qt6::Core Qt6::Network Qt6::WebSockets)
target_compile_definitions(wargame_serverlib PRIVATE
    WARGAME_VERSION="${WARGAME_VERSION}"
    WARGAME_SOURCE_DIGEST="${WARGAME_SOURCE_DIGEST}")
if(WARGAME_HAS_FASTDDS)
    target_compile_definitions(wargame_serverlib PUBLIC WARGAME_HAS_FASTDDS=1)
    target_link_libraries(wargame_serverlib PUBLIC ${WARGAME_FASTDDS_TARGET})
endif()
set_target_properties(wargame_serverlib PROPERTIES AUTOMOC ON)
wargame_enable_sanitizers(wargame_serverlib)

add_executable(wargame_server game/main.cpp)
target_link_libraries(wargame_server PRIVATE wargame_serverlib)
target_compile_definitions(wargame_server PRIVATE
    WARGAME_VERSION="${WARGAME_VERSION}"
    WARGAME_SOURCE_DIGEST="${WARGAME_SOURCE_DIGEST}")
wargame_enable_sanitizers(wargame_server)

install(TARGETS wargame_server RUNTIME DESTINATION bin)
