cmake_minimum_required(VERSION 3.16)
project(vmf_codec LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(vmf_encode src/vmf_encode.cpp)
target_link_libraries(vmf_encode PRIVATE tinyxml2)

add_executable(vmf_decode src/vmf_decode.cpp)
target_link_libraries(vmf_decode PRIVATE tinyxml2)

add_executable(vmf_validate src/vmf_validate.cpp)
target_link_libraries(vmf_validate PRIVATE tinyxml2)

add_executable(xml_compare src/xml_compare.cpp)
target_link_libraries(xml_compare PRIVATE tinyxml2)
