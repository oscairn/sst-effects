# Oscairn build (doc/oscairn-plan.md): pin the dependencies the test block below would otherwise
# take from `main`, so the plugin, its tests and the stock-vs-fork gate are reproducible.
# Included only with SST_EFFECTS_BUILD_OSCAIRN; upstream's own test build is unchanged.
include(${CMAKE_CURRENT_LIST_DIR}/CPM.cmake)

# The plugin is a shared module, so the static libraries it links (fmt) must be position independent.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

CPMAddPackage(NAME simde GITHUB_REPOSITORY simd-everywhere/simde VERSION 0.7.2)
add_library(simde INTERFACE)
target_include_directories(simde INTERFACE ${simde_SOURCE_DIR})

CPMAddPackage(NAME sst-basic-blocks GITHUB_REPOSITORY surge-synthesizer/sst-basic-blocks
        GIT_TAG 38cf04ae6a16994cc6c82635ce7c206f2a0ca31d)
CPMAddPackage(NAME sst-filters GITHUB_REPOSITORY surge-synthesizer/sst-filters
        GIT_TAG 87a171bcba52c014850848a29174a54f0adc6606)
CPMAddPackage(NAME sst-waveshapers GITHUB_REPOSITORY surge-synthesizer/sst-waveshapers
        GIT_TAG dd12f31a5a9016c9895e52d1a00eee0e1eebe6ce)
CPMAddPackage(NAME sst-plugininfra GITHUB_REPOSITORY surge-synthesizer/sst-plugininfra
        GIT_TAG 8e4271ce94039652c1cad3d89444c92325e0ce3e)
CPMAddPackage(NAME eurorack GITHUB_REPOSITORY surge-synthesizer/eurorack
        GIT_TAG 888298b0e3ec5daabf96c5ab912ffc42825a2b4c)
CPMAddPackage(NAME clap GITHUB_REPOSITORY free-audio/clap GIT_TAG 1.2.7)
