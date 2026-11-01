# Oscairn effects: sst-effects as CLAP plugins for the Oscairn host (doc/oscairn-plan.md).
# Built with -DSST_EFFECTS_BUILD_OSCAIRN=ON from the repository root.
project(oscairn-effects LANGUAGES CXX)

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

