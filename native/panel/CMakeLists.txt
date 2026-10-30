# CMakeLists.txt — Synth1 universal panel (U2): the M4 editor panel as portable C++17.
#
# No OS and no GUI-toolkit code: renders into RGBA buffers, consumes pointer/key events.
# The skin (captured art + behaviour data) is loaded from a directory at runtime.

cmake_minimum_required(VERSION 3.16)
project(synth1_panel LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(synth1_panel STATIC
  panel_util.cpp
  panel_core.cpp
  panel_widgets.cpp
  panel_popups.cpp
  panel_select.cpp
  ui_font.cpp
  panel.cpp
  skin.cpp
)
target_include_directories(synth1_panel PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
