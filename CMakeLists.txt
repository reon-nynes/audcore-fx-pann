cmake_minimum_required(VERSION 3.22)
project(PannFx VERSION 0.1.3 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_definitions(_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR)

set(pann_source "${CMAKE_CURRENT_LIST_DIR}/src")
set(pann_assets "${CMAKE_CURRENT_BINARY_DIR}/generated/local/PANN/Assets.h")
set(pann_inputs definition.json)
list(TRANSFORM pann_inputs PREPEND "${pann_source}/")

add_custom_command(OUTPUT "${pann_assets}"
 COMMAND "${CMAKE_COMMAND}" "-DOUT=${pann_assets}" "-DSOURCE=${pann_source}"
  -P "${pann_source}/Pack.cmake"
 DEPENDS "${pann_source}/Pack.cmake" ${pann_inputs}
 VERBATIM)

add_library(PANNLocalFx MODULE EXCLUDE_FROM_ALL
 "${pann_source}/Rack.cpp"
 "${pann_source}/Panel.cpp"
 "${pann_assets}")
target_include_directories(PANNLocalFx PRIVATE
 "${CMAKE_CURRENT_SOURCE_DIR}/include"
 "${CMAKE_CURRENT_SOURCE_DIR}/src"
 "${CMAKE_CURRENT_BINARY_DIR}/generated/local/PANN")
target_compile_definitions(PANNLocalFx PRIVATE
 AUDCORE_FX_EXPORTS AUDCORE_FX_RACK_ONLY
 NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE PANN_VERSION="${PROJECT_VERSION}")
target_compile_options(PANNLocalFx PRIVATE /utf-8 /W4 /Gy /Gw /Zc:inline /experimental:deterministic
 "/pathmap:${CMAKE_CURRENT_SOURCE_DIR}=pann")
target_link_options(PANNLocalFx PRIVATE /OPT:REF /OPT:ICF /INCREMENTAL:NO /DEBUG:NONE)
target_link_libraries(PANNLocalFx PRIVATE user32 gdi32 gdiplus comctl32)
set_target_properties(PANNLocalFx PROPERTIES
 PREFIX ""
 SUFFIX ".fx.audcore"
 OUTPUT_NAME "pann-v${PROJECT_VERSION}"
 MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
 RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_BINARY_DIR}/package"
 LIBRARY_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_BINARY_DIR}/package")

add_executable(PannTests EXCLUDE_FROM_ALL
 "${CMAKE_CURRENT_SOURCE_DIR}/tests/PannTests.cpp")
target_include_directories(PannTests PRIVATE
 "${CMAKE_CURRENT_SOURCE_DIR}/include"
 "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(PannTests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(PannTests PRIVATE /utf-8 /W4)
