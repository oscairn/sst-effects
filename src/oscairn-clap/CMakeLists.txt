# Oscairn effects: sst-effects as CLAP plugins for the Oscairn host (doc/oscairn-plan.md).
# Built with -DSST_EFFECTS_BUILD_OSCAIRN=ON from the repository root.
# The version here is the Oscairn PACKAGE version (the tag oscairn-vX.Y.Z and
# src/oscairn-clap/release/release.conf's release manifest must agree with it); the
# plugins' own versions live in their descriptors in OscairnEffects.cpp.
project(oscairn-effects VERSION 0.2.0 LANGUAGES CXX)

add_library(oscairn-effects-core STATIC OscairnEffects.cpp)
target_include_directories(oscairn-effects-core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../../libs/oscairn-clap/include)
target_link_libraries(oscairn-effects-core PUBLIC clap simde sst-basic-blocks sst-filters sst-waveshapers fmt sst-effects)
target_compile_definitions(oscairn-effects-core PUBLIC _USE_MATH_DEFINES=1)

add_library(oscairn-effects MODULE OscairnEffectsEntry.cpp)
target_link_libraries(oscairn-effects PRIVATE oscairn-effects-core)
set_target_properties(oscairn-effects PROPERTIES
  PREFIX ""
  SUFFIX ".clap"
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON)

add_executable(oscairn-effects-tests tests/OscairnEffectsTests.cpp)
target_link_libraries(oscairn-effects-tests PRIVATE oscairn-effects-core)
target_include_directories(oscairn-effects-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../libs/catch2)

# Opens a plugin as Oscairn does, for tests/privacy-check.sh: nothing may touch the user's
# Documents, Desktop or Downloads, or create anything under HOME.
add_executable(oscairn-effects-privacy-host tests/privacy-host.cpp)
target_link_libraries(oscairn-effects-privacy-host PRIVATE clap ${CMAKE_DL_LIBS})

# Runs a WAV file through a plugin, for measurements against reference renders (plan, section 11).
add_executable(oscairn-effects-render tools/OscairnEffectsRender.cpp)
target_link_libraries(oscairn-effects-render PRIVATE oscairn-effects-core)

# The stock-vs-fork gate (scripts/stock-gate.sh): the same render program against this branch's
# headers and, when OSCAIRN_STOCK_BASE_INCLUDE names an upstream include/ directory, against those.
add_executable(oscairn-stock-render tests/StockRender.cpp)
target_link_libraries(oscairn-stock-render PRIVATE oscairn-effects-core)
if (OSCAIRN_STOCK_BASE_INCLUDE)
  add_executable(oscairn-stock-render-base tests/StockRender.cpp)
  target_include_directories(oscairn-stock-render-base BEFORE PRIVATE ${OSCAIRN_STOCK_BASE_INCLUDE})
  target_include_directories(oscairn-stock-render-base PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(oscairn-stock-render-base PRIVATE clap simde sst-basic-blocks sst-filters sst-waveshapers fmt)
  target_compile_definitions(oscairn-stock-render-base PRIVATE _USE_MATH_DEFINES=1)
endif()
# Some stock effects use SSE4.1 intrinsics; upstream's test target builds with -march=native on
# Linux for the same reason. Both render programs get the same flags.
if (UNIX AND NOT APPLE)
  target_compile_options(oscairn-stock-render PRIVATE -march=native)
  if (TARGET oscairn-stock-render-base)
    target_compile_options(oscairn-stock-render-base PRIVATE -march=native)
  endif()
endif()
