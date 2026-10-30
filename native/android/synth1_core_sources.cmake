# Source lists shared by the Android NDK build and the Linux host render entry.
# core/ and panel/ are referenced IN PLACE (no copies). Same list/flags as
# portable-synth/CMakeLists.txt and plugin/CMakeLists.txt.
set(S1_ROOT ${CMAKE_CURRENT_LIST_DIR}/..)
set(S1_CORE ${S1_ROOT}/core)

# Clang (NDK) only: core/spec_constants.h:167 has static_assert(std::pow(...)), valid only as a GCC
# constexpr-builtin extension; clang rejects it. core/ must not be edited by this task, so for
# Clang the core is presented through a SYMLINK FARM in the build dir where only spec_constants.h
# is a generated copy with that single compile-time sanity assert dropped (everything else is the
# live core file; relative includes resolve inside the farm). GCC builds use core/ in place.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  set(S1_CORE ${CMAKE_BINARY_DIR}/core_farm)
  file(GLOB_RECURSE _s1_all RELATIVE ${S1_ROOT}/core ${S1_ROOT}/core/*)
  foreach(_f ${_s1_all})
    get_filename_component(_d ${_f} DIRECTORY)
    file(MAKE_DIRECTORY ${S1_CORE}/${_d})
    if(_f STREQUAL "spec_constants.h")
      file(READ ${S1_ROOT}/core/spec_constants.h _sc)
      string(REGEX REPLACE "static_assert\\(std::pow[^;]*;" "/* static_assert(std::pow ...) dropped: clang has no constexpr pow (GCC extension) */" _sc "${_sc}")
      file(WRITE ${S1_CORE}/spec_constants.h "${_sc}")
      set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${S1_ROOT}/core/spec_constants.h)
    else()
      file(CREATE_LINK ${S1_ROOT}/core/${_f} ${S1_CORE}/${_f} SYMBOLIC)
    endif()
  endforeach()
endif()
set(S1_CORE_SOURCES
  ${S1_CORE}/tables/wavetable_gen.cpp
  ${S1_CORE}/preset/preset.cpp
  ${S1_CORE}/env/adsr.cpp
  ${S1_CORE}/env/velocity_curve.cpp
  ${S1_CORE}/voice/voice.cpp
  ${S1_CORE}/voice/voice_allocator.cpp
  ${S1_CORE}/engine/engine.cpp
  ${S1_CORE}/engine/master_volume.cpp
  ${S1_CORE}/filter/filter.cpp
  ${S1_CORE}/lfo/lfo.cpp
  ${S1_CORE}/osc/osc_dispatch.cpp
  ${S1_CORE}/arp/arpeggiator.cpp
  ${S1_CORE}/midi/mod_wheel.cpp
  ${S1_CORE}/midi/cc_map.cpp
  ${S1_CORE}/fx/effector.cpp
  ${S1_CORE}/fx/delay.cpp
  ${S1_CORE}/fx/chorus.cpp
  ${S1_CORE}/fx/eq.cpp
  ${S1_CORE}/fx/pan.cpp
  ${S1_CORE}/fx/fx_chain.cpp
  ${S1_CORE}/settings/settings.cpp
)
set(S1_PANEL_SOURCES
  ${S1_ROOT}/panel/panel_util.cpp
  ${S1_ROOT}/panel/panel_core.cpp
  ${S1_ROOT}/panel/panel_widgets.cpp
  ${S1_ROOT}/panel/panel_popups.cpp
  ${S1_ROOT}/panel/panel_select.cpp
  ${S1_ROOT}/panel/ui_font.cpp
  ${S1_ROOT}/panel/panel.cpp
  ${S1_ROOT}/panel/skin.cpp
)
# Float law: -O2, FP contraction OFF, no fast-math (parity with the x86 host/plugin builds).
set(S1_FP_FLAGS -O2 -ffp-contract=off)
