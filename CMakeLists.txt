cmake_minimum_required(VERSION 3.22)
project(EvanPlugins VERSION 0.1.0 LANGUAGES C CXX)

# Three plug-ins in one repo. Each builds AU (macOS), VST3 and a Standalone app
# from the same processor/editor. Turn any of them off to build faster.
option(BUILD_CONTOUR_EQ   "Build Contour EQ"   ON)
option(BUILD_CAPTURE_RACK "Build Capture Rack" ON)
option(BUILD_OMNISAMPLER  "Build Omnisampler"  ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
endif()

include(cmake/Dependencies.cmake)

set(PLUGIN_FORMATS VST3 Standalone)
if(APPLE)
    list(APPEND PLUGIN_FORMATS AU)
endif()

# Settings every plug-in shares.
function(evan_plugin_defaults target)
    target_include_directories(${target} PRIVATE ${CMAKE_SOURCE_DIR}/Shared)
    target_compile_definitions(${target} PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_DISPLAY_SPLASH_SCREEN=0
        JUCE_REPORT_APP_USAGE=0)
    target_link_libraries(${target}
        PRIVATE juce::juce_audio_utils juce::juce_dsp
        PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_lto_flags
               juce::juce_recommended_warning_flags)
endfunction()

if(BUILD_CONTOUR_EQ)
    add_subdirectory(ContourEQ)
endif()
if(BUILD_CAPTURE_RACK)
    add_subdirectory(CaptureRack)
endif()
if(BUILD_OMNISAMPLER)
    add_subdirectory(Omnisampler)
endif()

option(BUILD_TESTS "Build the headless test programs" OFF)
if(BUILD_TESTS)
    add_subdirectory(Tests)
endif()
