cmake_minimum_required(VERSION 3.20)
project(framegen LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "framegen targets Windows (Desktop Duplication, D3D11, DirectComposition)")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(framegen
  src/main.cpp
  src/capture_dda.cpp
  src/pipeline.cpp
  src/presenter.cpp)

target_compile_definitions(framegen PRIVATE UNICODE _UNICODE)
target_link_libraries(framegen PRIVATE d3d11 dxgi d3dcompiler dcomp user32)

if(MSVC)
  target_compile_options(framegen PRIVATE /W4 /permissive-)
else()
  target_compile_options(framegen PRIVATE -Wall -Wextra)
  target_link_options(framegen PRIVATE -municode -static)
endif()

# Shaders are compiled at startup, so ship the source next to the executable,
# along with the starter profile file.
add_custom_command(TARGET framegen POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${CMAKE_CURRENT_SOURCE_DIR}/shaders $<TARGET_FILE_DIR:framegen>/shaders
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_CURRENT_SOURCE_DIR}/profiles.ini $<TARGET_FILE_DIR:framegen>/profiles.ini)
