cmake_minimum_required(VERSION 3.22)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(wargame_server VERSION 0.1.0 LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    option(WARGAME_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
    find_package(Qt6 6.4 REQUIRED COMPONENTS Core Network WebSockets)
    include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/WargameTargets.cmake)
    wargame_add_domain_library(wargame_domain ${CMAKE_CURRENT_SOURCE_DIR}/..)
    wargame_add_protocol_library(wargame_protocol ${CMAKE_CURRENT_SOURCE_DIR}/..)
    wargame_enable_sanitizers(wargame_domain)
    wargame_enable_sanitizers(wargame_protocol)
endif()

if(NOT DEFINED WARGAME_VERSION)
    set(WARGAME_VERSION "${PROJECT_VERSION}" CACHE STRING
        "Version embedded in server release artifacts")
endif()

add_library(wargame_serverlib STATIC
    game/GameServer.h
    game/GameServer.cpp
    game/RoomPersistence.h
    game/RoomPersistence.cpp
    game/StateProjector.h
    game/StateProjector.cpp
)
target_include_directories(wargame_serverlib PUBLIC ../src game)
target_link_libraries(wargame_serverlib PUBLIC
    wargame_domain wargame_protocol Qt6::Core Qt6::Network Qt6::WebSockets)
set_target_properties(wargame_serverlib PROPERTIES AUTOMOC ON)
wargame_enable_sanitizers(wargame_serverlib)

add_executable(wargame_server game/main.cpp)
target_link_libraries(wargame_server PRIVATE wargame_serverlib)
target_compile_definitions(wargame_server PRIVATE
    WARGAME_VERSION="${WARGAME_VERSION}")
wargame_enable_sanitizers(wargame_server)

install(TARGETS wargame_server RUNTIME DESTINATION bin)
