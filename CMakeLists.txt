cmake_minimum_required(VERSION 3.24)
project(prtxrtxpt VERSION 1.0 LANGUAGES CXX RC)

if(NOT WIN32 OR NOT MSVC)
  message(FATAL_ERROR "Use Visual Studio with the Windows SDK and C++/WinRT headers.")
endif()

find_package(Vulkan REQUIRED COMPONENTS glslc)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(shader_headers)
foreach(stage vert frag)
  set(output "${CMAKE_CURRENT_BINARY_DIR}/map.${stage}.inc")
  add_custom_command(
    OUTPUT "${output}"
    COMMAND Vulkan::glslc -O -mfmt=c "${CMAKE_CURRENT_SOURCE_DIR}/shaders/map.${stage}" -o "${output}"
    DEPENDS "shaders/map.${stage}"
    VERBATIM)
  list(APPEND shader_headers "${output}")
endforeach()

add_library(map_core OBJECT
  src/world/map.cpp
  src/world/mesh.cpp
)
target_include_directories(map_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(map_core PUBLIC cxx_std_20)
target_compile_definitions(map_core PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
target_link_libraries(map_core PUBLIC windowsapp)

add_executable(prtxrtxpt WIN32
  src/app/main.cpp
  src/app/viewer.rc
  src/renderer/renderer.cpp
  ${shader_headers}
)
target_compile_definitions(prtxrtxpt PRIVATE VK_USE_PLATFORM_WIN32_KHR)
target_include_directories(prtxrtxpt PRIVATE
  "${CMAKE_CURRENT_BINARY_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(prtxrtxpt PRIVATE map_core Vulkan::Vulkan comctl32)

option(BUILD_TESTING "Build automated map tests" OFF)
if(BUILD_TESTING)
  include(CTest)
  add_executable(map_tests tests/map_tests.cpp)
  target_link_libraries(map_tests PRIVATE map_core)
  add_test(NAME map_format_and_mesh COMMAND map_tests "${CMAKE_CURRENT_SOURCE_DIR}/assets/maps")
endif()
