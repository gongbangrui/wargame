cmake_minimum_required(VERSION 3.16)
project(vmf_codec LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(vmf_encode src/vmf_encode.cpp)
target_link_libraries(vmf_encode PRIVATE tinyxml2)

add_executable(vmf_decode src/vmf_decode.cpp)
target_link_libraries(vmf_decode PRIVATE tinyxml2)

# Make the no-argument Qt Creator Run action execute a harmless smoke test.
# Generated files stay in the build tree; the normal four-argument CLI path
# remains unchanged for real input files.
set(VMF_DECODE_DEFAULT_STRUCTURE "${CMAKE_CURRENT_BINARY_DIR}/vmf_decode_default_structure.xml")
set(VMF_DECODE_DEFAULT_CONTENT "${CMAKE_CURRENT_BINARY_DIR}/vmf_decode_default_content.xml")
set(VMF_DEFAULT_MESSAGE "${CMAKE_CURRENT_BINARY_DIR}/vmf_default_message.xml")
set(VMF_DECODE_DEFAULT_INPUT "${CMAKE_CURRENT_BINARY_DIR}/vmf_decode_default.bin")
configure_file(msgStruct/msg0_1.xml "${VMF_DECODE_DEFAULT_STRUCTURE}" COPYONLY)
configure_file(dic_content.xml "${VMF_DECODE_DEFAULT_CONTENT}" COPYONLY)
configure_file(msg_pass.xml "${VMF_DEFAULT_MESSAGE}" COPYONLY)
add_custom_command(
  OUTPUT "${VMF_DECODE_DEFAULT_INPUT}"
  COMMAND vmf_encode "${VMF_DECODE_DEFAULT_STRUCTURE}" "${VMF_DECODE_DEFAULT_CONTENT}"
          "${VMF_DEFAULT_MESSAGE}" "${VMF_DECODE_DEFAULT_INPUT}"
  DEPENDS vmf_encode "${VMF_DECODE_DEFAULT_STRUCTURE}" "${VMF_DECODE_DEFAULT_CONTENT}"
          "${VMF_DEFAULT_MESSAGE}"
  COMMENT "Generating vmf_decode default smoke-test input"
  VERBATIM)
add_custom_target(vmf_decode_default_input DEPENDS "${VMF_DECODE_DEFAULT_INPUT}")
add_dependencies(vmf_decode vmf_decode_default_input)

set(VMF_COMPARE_DEFAULT_A "${CMAKE_CURRENT_BINARY_DIR}/vmf_compare_default_a.xml")
set(VMF_COMPARE_DEFAULT_B "${CMAKE_CURRENT_BINARY_DIR}/vmf_compare_default_b.xml")
configure_file(msg_pass.xml "${VMF_COMPARE_DEFAULT_A}" COPYONLY)
configure_file(msg_pass.xml "${VMF_COMPARE_DEFAULT_B}" COPYONLY)

add_executable(vmf_validate src/vmf_validate.cpp)
target_link_libraries(vmf_validate PRIVATE tinyxml2)

add_executable(xml_compare src/xml_compare.cpp)
target_link_libraries(xml_compare PRIVATE tinyxml2)
