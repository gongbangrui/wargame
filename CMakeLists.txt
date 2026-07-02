cmake_minimum_required(VERSION 3.16)

project(index VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Quick QuickControls2)

qt_standard_project_setup(REQUIRES 6.10)

if (EXISTS "${CMAKE_CURRENT_LIST_DIR}/importedcontent/CMakeLists.txt")
    add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/importedcontent)
endif()

qt_add_executable(appindex
    main.cpp
    src/core/SimulationEngine.h
    src/core/SimulationEngine.cpp
    src/core/UnitBase.h
    src/core/UnitBase.cpp
    src/core/MessageBus.h
    src/core/MessageBus.cpp
    src/core/Scenario.h
    src/core/Scenario.cpp
    src/core/Geo.h
    src/core/Geo.cpp
    src/core/MapProvider.h
    src/core/MapProvider.cpp
    src/units/CommandPost.h
    src/units/CommandPost.cpp
    src/units/ReconUAV.h
    src/units/ReconUAV.cpp
    src/units/AttackUAV.h
    src/units/AttackUAV.cpp
    src/units/GroundScout.h
    src/units/GroundScout.cpp
    src/view/SimulationController.h
    src/view/SimulationController.cpp
    src/view/ScenarioEditor.h
    src/view/ScenarioEditor.cpp
)

qt_add_qml_module(appindex
    URI index
    QML_FILES
        Main.qml
        qml/SimulationRoot.qml
        qml/views/CommandPostView.qml
        qml/views/ReconUavView.qml
        qml/views/AttackUavView.qml
        qml/views/GroundScoutView.qml
        qml/views/DirectorView.qml
        qml/views/ScenarioEditorView.qml
        qml/components/MapCanvas.qml
        qml/components/MessageLog.qml
        qml/components/EventDialog.qml
        qml/components/UnitPanel.qml
        qml/components/SectionTitle.qml
        qml/components/TonalButton.qml
        qml/components/GhostButton.qml
        qml/components/UnitEditDialog.qml
        qml/components/RoutePlannerDialog.qml
    RESOURCES
        assets/sample_map.json
)

set_target_properties(appindex PROPERTIES
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
    MACOSX_BUNDLE TRUE
    WIN32_EXECUTABLE TRUE
)

target_link_libraries(appindex
    PRIVATE Qt6::Quick Qt6::QuickControls2
)

install(TARGETS appindex
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)



