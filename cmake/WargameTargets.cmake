include_guard(GLOBAL)

function(wargame_add_domain_library target source_root)
    add_library(${target} STATIC
        ${source_root}/src/core/SimulationEngine.h
        ${source_root}/src/core/SimulationEngine.cpp
        ${source_root}/src/core/CommandResult.h
        ${source_root}/src/core/CommandResult.cpp
        ${source_root}/src/core/UnitBase.h
        ${source_root}/src/core/UnitBase.cpp
        ${source_root}/src/core/MessageBus.h
        ${source_root}/src/core/MessageBus.cpp
        ${source_root}/src/core/Scenario.h
        ${source_root}/src/core/Scenario.cpp
        ${source_root}/src/core/Geo.h
        ${source_root}/src/core/MapProvider.h
        ${source_root}/src/core/MapProvider.cpp
        ${source_root}/src/core/UnitFsm.h
        ${source_root}/src/core/UnitFsm.cpp
        ${source_root}/src/core/ITransport.h
        ${source_root}/src/core/LocalTransport.h
        ${source_root}/src/core/LocalTransport.cpp
        ${source_root}/src/core/IClock.h
        ${source_root}/src/core/RealTimeClock.h
        ${source_root}/src/core/MessageLogRecorder.h
        ${source_root}/src/core/MessageLogRecorder.cpp
        ${source_root}/src/core/SnapshotCodec.h
        ${source_root}/src/core/SnapshotCodec.cpp
        ${source_root}/src/core/LockedStepClock.h
        ${source_root}/src/core/LockedStepClock.cpp
        ${source_root}/src/units/CommandPost.h
        ${source_root}/src/units/CommandPost.cpp
        ${source_root}/src/units/MobileUnitBase.h
        ${source_root}/src/units/MobileUnitBase.cpp
        ${source_root}/src/units/ReconUAV.h
        ${source_root}/src/units/ReconUAV.cpp
        ${source_root}/src/units/AttackUAV.h
        ${source_root}/src/units/AttackUAV.cpp
        ${source_root}/src/units/GroundScout.h
        ${source_root}/src/units/GroundScout.cpp
        ${source_root}/src/units/JammerUAV.h
        ${source_root}/src/units/JammerUAV.cpp
    )
    target_link_libraries(${target} PUBLIC Qt6::Core)
    target_include_directories(${target} PUBLIC ${source_root}/src)
    set_target_properties(${target} PROPERTIES AUTOMOC ON)
endfunction()

function(wargame_add_protocol_library target source_root)
    add_library(${target} STATIC
        ${source_root}/src/protocol/Protocol.h
        ${source_root}/src/protocol/Protocol.cpp
        ${source_root}/src/protocol/StateDelta.h
        ${source_root}/src/protocol/StateDelta.cpp
    )
    target_link_libraries(${target} PUBLIC Qt6::Core)
    target_include_directories(${target} PUBLIC ${source_root}/src)
endfunction()

function(wargame_enable_sanitizers target)
    if(NOT WARGAME_ENABLE_SANITIZERS)
        return()
    endif()
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${target} PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    else()
        message(WARNING "WARGAME_ENABLE_SANITIZERS is not supported by this compiler")
    endif()
endfunction()
