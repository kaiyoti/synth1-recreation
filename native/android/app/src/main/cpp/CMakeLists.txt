# NDK build of the Synth1 Android shell. core/ and panel/ are compiled IN PLACE from
# portable-synth (no copies); Oboe is vendored (third_party/oboe, tag 1.9.3).
cmake_minimum_required(VERSION 3.22)
project(synth1_android CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../../../synth1_core_sources.cmake)
# Float law identical to the desktop builds: -O2 -ffp-contract=off, no fast-math.
add_compile_options(${S1_FP_FLAGS})

add_library(s1_core STATIC ${S1_CORE_SOURCES})
target_include_directories(s1_core PUBLIC ${S1_CORE})
add_library(s1_panel STATIC ${S1_PANEL_SOURCES})
target_include_directories(s1_panel PUBLIC ${S1_ROOT}/panel)

set(OBOE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../../third_party/oboe)
add_subdirectory(${OBOE_DIR} oboe-build)

add_library(s1_keyboard STATIC ${S1_ROOT}/keyboard/keyboard.cpp)
target_include_directories(s1_keyboard PUBLIC ${S1_ROOT}/keyboard)

add_library(synth1 SHARED android_main.cpp synth_host.cpp panel_view.cpp input_router.cpp)
target_include_directories(synth1 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${OBOE_DIR}/include)
target_link_libraries(synth1 s1_core s1_panel s1_keyboard oboe android log)
# 16 KB page-size alignment (Android 15+ devices)
target_link_options(synth1 PRIVATE -Wl,-z,max-page-size=16384)
