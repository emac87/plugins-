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
if(APPLE AND NOT IOS AND NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
endif()

include(cmake/Dependencies.cmake)

# iPad/iPhone (cmake -G Xcode -DCMAKE_SYSTEM_NAME=iOS): AUv3 inside a Standalone app, for TestFlight.
set(EVAN_TEAM_ID "" CACHE STRING "Apple Development Team ID (iOS signing)")
set(EVAN_BUILD "1" CACHE STRING "Build number (CFBundleVersion); must increase for each TestFlight upload")
if(IOS)
    set(PLUGIN_FORMATS AUv3 Standalone)
    set(EVAN_BUNDLE_PREFIX "com.evanmcintosh")   # App Store bundle IDs (team 4YRKXPJ3P5)
else()
    set(PLUGIN_FORMATS VST3 Standalone)
    if(APPLE)
        list(APPEND PLUGIN_FORMATS AU)
    endif()
    set(EVAN_BUNDLE_PREFIX "com.emac87")
endif()
# Options every plug-in passes to juce_add_plugin (ignored where they don't apply).
set(EVAN_COMMON_OPTIONS
    BUILD_VERSION "${EVAN_BUILD}"
    DEVELOPMENT_TEAM_ID "${EVAN_TEAM_ID}"
    TARGETED_DEVICE_FAMILY "1,2"
    BACKGROUND_AUDIO_ENABLED TRUE
    FILE_SHARING_ENABLED TRUE
    DOCUMENT_BROWSER_ENABLED TRUE
    REQUIRES_FULL_SCREEN FALSE
    APP_GROUPS_ENABLED FALSE
    IPHONE_SCREEN_ORIENTATIONS UIInterfaceOrientationLandscapeLeft UIInterfaceOrientationLandscapeRight
    IPAD_SCREEN_ORIENTATIONS UIInterfaceOrientationPortrait UIInterfaceOrientationPortraitUpsideDown UIInterfaceOrientationLandscapeLeft UIInterfaceOrientationLandscapeRight)

# iOS archive: only the Standalone app goes into the archive (the AUv3 extension is embedded in it).
function(evan_ios_archive name)
    if(IOS)
        set_target_properties(${name} ${name}_AUv3 PROPERTIES XCODE_ATTRIBUTE_SKIP_INSTALL YES)
        set_target_properties(${name}_Standalone PROPERTIES XCODE_ATTRIBUTE_SKIP_INSTALL NO XCODE_ATTRIBUTE_INSTALL_PATH "$(LOCAL_APPS_DIR)")
    endif()
endfunction()

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
