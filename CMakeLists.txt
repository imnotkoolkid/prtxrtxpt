cmake_minimum_required(VERSION 3.24)
project(prtxrtxpt VERSION 1.0 LANGUAGES CXX RC)

if(NOT WIN32 OR NOT MSVC)
  message(FATAL_ERROR "Use Visual Studio with the Windows SDK and C++/WinRT headers.")
endif()

find_package(Vulkan 1.3 REQUIRED COMPONENTS glslc)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ---------- shaders ----------------------------------------------------------
set(shader_headers)
file(GLOB shader_includes CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.glsl")
foreach(stage map.vert map.frag trace.comp filter.comp present.vert present.frag)
  set(output "${CMAKE_CURRENT_BINARY_DIR}/${stage}.inc")
  add_custom_command(
    OUTPUT "${output}"
    COMMAND Vulkan::glslc --target-env=vulkan1.3 --target-spv=spv1.5 -O -mfmt=c "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${stage}" -o "${output}"
    DEPENDS "shaders/${stage}" ${shader_includes}
    VERBATIM)
  list(APPEND shader_headers "${output}")
endforeach()

# ---------- libraries --------------------------------------------------------
add_library(map_core OBJECT
  src/world/map.cpp
  src/world/mesh.cpp
  src/world/trace_scene.cpp
)
target_include_directories(map_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(map_core PUBLIC cxx_std_20)
target_compile_definitions(map_core PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
target_link_libraries(map_core PUBLIC windowsapp)

add_library(trace_renderer STATIC src/renderer/trace.cpp ${shader_headers})
target_include_directories(trace_renderer PUBLIC "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(trace_renderer PUBLIC map_core Vulkan::Vulkan)
target_compile_definitions(trace_renderer PUBLIC VK_USE_PLATFORM_WIN32_KHR)

# ---------- Discord Rich Presence config -------------------------------------
set(PRTX_DISCORD_APP_ID "" CACHE STRING "Discord Application ID for Rich Presence")
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/src/app/discord_config.hpp.in"
  "${CMAKE_CURRENT_BINARY_DIR}/discord_config.hpp"
  @ONLY)

# ---------- executable -------------------------------------------------------
add_executable(prtxrtxpt WIN32
  src/app/main.cpp
  src/app/discord_rpc.cpp
  src/app/viewer.rc
  src/renderer/renderer.cpp
  src/app/settings_window.cpp
)
target_include_directories(prtxrtxpt PRIVATE
  "${CMAKE_CURRENT_BINARY_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(prtxrtxpt PRIVATE trace_renderer comctl32 comdlg32)
