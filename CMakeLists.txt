cmake_minimum_required(VERSION 3.25)

project(OsuLazerAudioBridge
    VERSION 0.2.0
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
    message(FATAL_ERROR "OsuLazerAudioBridge is Windows-only.")
endif()

option(OLAB_FETCH_MINHOOK "Fetch MinHook with CMake FetchContent" ON)

add_library(OsuLazerAudioShared INTERFACE)
target_include_directories(OsuLazerAudioShared INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/native/shared/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/audio_decoders)

add_library(OsuLazerAsioSdk STATIC
    third_party/asio/host/asiodrivers.cpp
    third_party/asio/host/pc/asiolist.cpp)
target_include_directories(OsuLazerAsioSdk PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/asio/common
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/asio/host
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/asio/host/pc)
target_compile_definitions(OsuLazerAsioSdk PUBLIC
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    WINDOWS=1)

if(OLAB_FETCH_MINHOOK)
    include(FetchContent)
    FetchContent_Declare(
        minhook
        GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
        GIT_TAG v1.3.4)
    FetchContent_MakeAvailable(minhook)
elseif(NOT TARGET minhook)
    message(FATAL_ERROR "OLAB_FETCH_MINHOOK is OFF, but no minhook target is available.")
endif()

add_library(OsuLazerBassHook SHARED
    native/hook/BassHook.cpp)
target_link_libraries(OsuLazerBassHook PRIVATE
    OsuLazerAudioShared
    minhook)
target_compile_definitions(OsuLazerBassHook PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX)

add_executable(OsuLazerAudioHost
    native/host/AudioHost.cpp
    native/host/HostAudioOutput.cpp
    native/host/HostAsioDevices.cpp
    native/host/HostLogging.cpp
    native/host/HostNativeAsioOutput.cpp
    native/host/HostSharedChannel.cpp
    native/host/HostWasapiOutput.cpp
    native/host/HostWin32.cpp
    native/host/HostXAudio2Output.cpp
    native/host/SampleDecoder.cpp)
target_link_libraries(OsuLazerAudioHost PRIVATE
    OsuLazerAudioShared
    OsuLazerAsioSdk
    xaudio2
    ole32
    avrt
    uuid)
target_compile_definitions(OsuLazerAudioHost PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE)

add_executable(OsuLazerAudioBridge WIN32
    native/gui/AudioBridgeGui.cpp)
target_link_libraries(OsuLazerAudioBridge PRIVATE
    OsuLazerAudioShared
    ole32
    uuid
    shell32
    comctl32)
target_compile_definitions(OsuLazerAudioBridge PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE)

set_target_properties(OsuLazerBassHook OsuLazerAudioHost OsuLazerAudioBridge PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
