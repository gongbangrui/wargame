if(NOT DEFINED WARGAME_FASTDDSGEN_EXECUTABLE
   OR NOT DEFINED WARGAME_DDS_GENERATED_DIR
   OR NOT DEFINED WARGAME_DDS_IDL)
    message(FATAL_ERROR "Fast DDS generation arguments are incomplete")
endif()

execute_process(
    COMMAND "${WARGAME_FASTDDSGEN_EXECUTABLE}" -replace -d "${WARGAME_DDS_GENERATED_DIR}"
            "${WARGAME_DDS_IDL}"
    RESULT_VARIABLE generator_result
    OUTPUT_VARIABLE generator_output
    ERROR_VARIABLE generator_error
)
if(NOT generator_result EQUAL 0)
    message(FATAL_ERROR
        "fastddsgen failed (${generator_result})\n${generator_output}\n${generator_error}")
endif()

# Fast DDS Gen 2.x and 4.x emit different support-file sets. Keep the build
# graph stable and let the generated PubSubTypes source include the matching
# header produced by the installed generator.
foreach(relative_path IN ITEMS
        WargameEnvelope.cxx
        WargameEnvelopePubSubTypes.cxx
        WargameEnvelopeTypeObjectSupport.cxx)
    set(path "${WARGAME_DDS_GENERATED_DIR}/${relative_path}")
    if(NOT EXISTS "${path}")
        file(TOUCH "${path}")
    endif()
endforeach()
