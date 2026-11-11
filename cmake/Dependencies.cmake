# JUCE 8, plus NeuralAmpModelerCore and Eigen for Capture Rack.
# Point the *_DIR cache variables at local checkouts to skip downloading.
include(FetchContent)

set(JUCE_DIR_LOCAL  "" CACHE PATH "Path to a local JUCE checkout (optional)")
set(NAM_DIR_LOCAL   "" CACHE PATH "Path to a local NeuralAmpModelerCore checkout (optional)")
set(EIGEN_DIR_LOCAL "" CACHE PATH "Path to a local Eigen include dir (optional)")

if(JUCE_DIR_LOCAL)
    add_subdirectory(${JUCE_DIR_LOCAL} ${CMAKE_BINARY_DIR}/juce EXCLUDE_FROM_ALL)
else()
    FetchContent_Declare(juce GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
                              GIT_TAG 8.0.4 GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(juce)
endif()

if(BUILD_CAPTURE_RACK)
    if(NAM_DIR_LOCAL)
        set(NAM_SOURCE_DIR ${NAM_DIR_LOCAL})
    else()
        # Newest NAM core (supports A1 and A2 models). Pinned so builds are repeatable.
        FetchContent_Declare(namcore GIT_REPOSITORY https://github.com/sdatkinson/NeuralAmpModelerCore.git
                                     GIT_TAG d3189ed482ded880c128bf312941266c4fa5ceec
                                     SOURCE_SUBDIR _none_)
        FetchContent_MakeAvailable(namcore)
        set(NAM_SOURCE_DIR ${namcore_SOURCE_DIR})
    endif()

    if(EIGEN_DIR_LOCAL)
        set(EIGEN_INCLUDE_DIR ${EIGEN_DIR_LOCAL})
    else()
        FetchContent_Declare(eigen GIT_REPOSITORY https://gitlab.com/libeigen/eigen.git
                                   GIT_TAG 3.4.0 GIT_SHALLOW TRUE
                                   SOURCE_SUBDIR _none_)
        FetchContent_MakeAvailable(eigen)
        set(EIGEN_INCLUDE_DIR ${eigen_SOURCE_DIR})
    endif()
endif()
