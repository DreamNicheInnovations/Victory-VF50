cmake_minimum_required(VERSION 3.22)

if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS deployment target")
endif()

project(VictoryVF50 VERSION 3.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ==============================================================================
# Build Options & Dependency Resolution (JUCE 8 + clap-juce-extensions)
# ==============================================================================
set(VF50_JUCE_DIR "" CACHE PATH "Optional path to a local JUCE source directory")
set(VF50_CLAP_EXT_DIR "" CACHE PATH "Optional path to a local clap-juce-extensions source directory")
option(VF50_DEPLOY_LOCAL "Deploy built plugins to local system plugin directories after build" OFF)

set(_vf50_juce_candidates
    "${VF50_JUCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/JUCE"
)

set(_vf50_found_juce "")
foreach(_cand IN LISTS _vf50_juce_candidates)
    if(NOT "${_cand}" STREQUAL "" AND EXISTS "${_cand}/CMakeLists.txt" AND EXISTS "${_cand}/modules/juce_audio_processors")
        get_filename_component(_vf50_found_juce "${_cand}" ABSOLUTE)
        break()
    endif()
endforeach()

if(NOT "${_vf50_found_juce}" STREQUAL "")
    message(STATUS "[VF50] Using local JUCE installation at: ${_vf50_found_juce}")
    add_subdirectory("${_vf50_found_juce}" "${CMAKE_BINARY_DIR}/JUCE" EXCLUDE_FROM_ALL)
else()
    message(STATUS "[VF50] Fetching JUCE 8.0.4 via FetchContent...")
    include(FetchContent)
    FetchContent_Declare(
        JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG        8.0.4
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(JUCE)
endif()

set(CLAP_JUCE_EXTENSIONS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(_vf50_clap_candidates
    "${VF50_CLAP_EXT_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/clap-juce-extensions"
)

set(_vf50_found_clap "")
foreach(_cand IN LISTS _vf50_clap_candidates)
    if(NOT "${_cand}" STREQUAL "" AND EXISTS "${_cand}/CMakeLists.txt" AND EXISTS "${_cand}/clap-libs/clap/CMakeLists.txt")
        get_filename_component(_vf50_found_clap "${_cand}" ABSOLUTE)
        break()
    endif()
endforeach()

if(NOT "${_vf50_found_clap}" STREQUAL "")
    message(STATUS "[VF50] Using local clap-juce-extensions at: ${_vf50_found_clap}")
    add_subdirectory("${_vf50_found_clap}" "${CMAKE_BINARY_DIR}/clap-juce-extensions" EXCLUDE_FROM_ALL)
else()
    message(STATUS "[VF50] Fetching clap-juce-extensions via FetchContent...")
    include(FetchContent)
    FetchContent_Declare(
        clap_juce_extensions
        GIT_REPOSITORY https://github.com/free-audio/clap-juce-extensions.git
        GIT_TAG        3837f4a6ed2ab0ba46aba69ae5b7041c7c64ce74
        GIT_SUBMODULES_RECURSE TRUE
    )
    FetchContent_MakeAvailable(clap_juce_extensions)
endif()

# ==============================================================================
# Shared C++20 Core Hardware DSP, ROM Bank, Memory State & SysEx Sources
# ==============================================================================
set(VF50_DSP_SOURCES
    Source/ROM/VF50DataTypes.h
    Source/ROM/VF50RomBank.h
    Source/ROM/VF50RomBank.cpp
    Source/DSP/FM/OpzVoice.cpp
    Source/DSP/FM/VF50FmEngine.cpp
    Source/DSP/Drums/Ym3602DrumEngine.cpp
    Source/DSP/Effects/Ym3413DspProcessor.cpp
    Source/DSP/VF50SynthEngine.cpp
    Source/State/VF50MemoryManager.cpp
    Source/State/VF50StateMachine.cpp
    Source/MIDI/VF50SysExHandler.cpp
)

add_library(VF50_DSP OBJECT ${VF50_DSP_SOURCES})
set_target_properties(VF50_DSP PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(VF50_DSP PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Source")
target_compile_definitions(VF50_DSP PUBLIC NOMINMAX _USE_MATH_DEFINES VF50_EMBEDDED_ROMS=0)
if(MSVC)
    target_compile_options(VF50_DSP PRIVATE /W4 /WX /MP /permissive- /EHsc /fp:fast)
else()
    target_compile_options(VF50_DSP PRIVATE -Wall -Wextra)
endif()

set(VF50_COMMON_PLUGIN_SOURCES
    Source/ROM/VF50RomLoader.h
    Source/ROM/VF50RomLoader.cpp
    Source/Disk/VF50DiskManager.h
    Source/Disk/VF50DiskManager.cpp
    Source/Disk/VF50MidiExporter.h
    Source/Disk/VF50MidiExporter.cpp
    Source/GUI/VF50DesignTokens.h
    Source/GUI/VF50LcdDisplay.h
    Source/GUI/VF50LcdDisplay.cpp
    Source/GUI/VF50LookAndFeel.h
    Source/GUI/VF50LookAndFeel.cpp
    Source/GUI/VF50MixerDrawer.h
    Source/GUI/VF50MixerDrawer.cpp
    Source/GUI/VF50ProgrammerDrawer.h
    Source/GUI/VF50ProgrammerDrawer.cpp
    Source/GUI/VF50RomGuideOverlay.h
    Source/GUI/VF50RomGuideOverlay.cpp
    Source/GUI/VF50VirtualKeyboard.h
    Source/GUI/VF50VirtualKeyboard.cpp
    Source/Classic/VF50ClassicTypes.h
    Source/Classic/VF50ClassicController.h
    Source/Classic/VF50ClassicController.cpp
    Source/Classic/VF50ClassicEditScreens.cpp
    Source/Classic/VF50ClassicRhythmScreens.cpp
    Source/Classic/VF50ClassicUtilScreens.cpp
    Source/State/VF50ClassicStateMachine.h
    Source/GUI/VF50MainCanvas.h
    Source/GUI/VF50MainCanvas.cpp
)

# ==============================================================================
# Platform Plugin Formats (VST3, CLAP, AU, LV2, Standalone)
# ==============================================================================
if(WIN32)
    set(VF50_PLUGIN_FORMATS VST3 Standalone)
    set(VF50_MULTI_PLUGIN_FORMATS VST3)
elseif(APPLE)
    set(VF50_PLUGIN_FORMATS AU VST3 Standalone)
    set(VF50_MULTI_PLUGIN_FORMATS AU VST3)
else()
    set(VF50_PLUGIN_FORMATS VST3 LV2 Standalone)
    set(VF50_MULTI_PLUGIN_FORMATS VST3 LV2)
endif()

function(vf50_configure_plugin_target target_name multi_out_val clap_id)
    target_sources(${target_name} PRIVATE
        Source/Plugin/PluginProcessor.cpp
        Source/Plugin/PluginProcessor.h
        Source/Plugin/PluginEditor.cpp
        Source/Plugin/PluginEditor.h
        ${VF50_COMMON_PLUGIN_SOURCES}
        $<TARGET_OBJECTS:VF50_DSP>
    )
    target_include_directories(${target_name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Source")
    target_compile_definitions(${target_name} PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        NOMINMAX
        _USE_MATH_DEFINES
        VF50_EMBEDDED_ROMS=0
        VF50_MULTI_OUT=${multi_out_val}
    )
    if(MSVC)
        target_compile_options(${target_name} PRIVATE /W4 /WX /MP /permissive- /EHsc)
    else()
        target_compile_options(${target_name} PRIVATE -Wall -Wextra)
    endif()
    target_link_libraries(${target_name}
        PRIVATE
            juce::juce_audio_utils
            juce::juce_audio_processors
            juce::juce_dsp
            juce::juce_gui_extra
        PUBLIC
            juce::juce_recommended_config_flags
    )

    clap_juce_extensions_plugin(
        TARGET ${target_name}
        CLAP_ID "${clap_id}"
        CLAP_FEATURES instrument synthesizer
    )

    if(WIN32 AND VF50_DEPLOY_LOCAL)
        set(_vf50_win_clap_dir "$ENV{CommonProgramFiles}/CLAP")
        add_custom_command(TARGET ${target_name}_CLAP POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "${_vf50_win_clap_dir}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:${target_name}_CLAP>" "${_vf50_win_clap_dir}/"
            VERBATIM
        )
    endif()
endfunction()

function(vf50_configure_headless_target target_name multi_out_val)
    target_sources(${target_name} PRIVATE
        Tests/VF50_HeadlessTest.cpp
        Source/Plugin/PluginProcessor.cpp
        Source/Plugin/PluginProcessor.h
        Source/Plugin/PluginEditor.cpp
        Source/Plugin/PluginEditor.h
        ${VF50_COMMON_PLUGIN_SOURCES}
        $<TARGET_OBJECTS:VF50_DSP>
    )
    target_include_directories(${target_name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Source")
    target_compile_definitions(${target_name} PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        NOMINMAX
        _USE_MATH_DEFINES
        VF50_EMBEDDED_ROMS=0
        VF50_MULTI_OUT=${multi_out_val}
    )
    if(MSVC)
        target_compile_options(${target_name} PRIVATE /W4 /WX /MP /permissive- /EHsc)
    else()
        target_compile_options(${target_name} PRIVATE -Wall -Wextra)
    endif()
    target_link_libraries(${target_name}
        PRIVATE
            juce::juce_audio_utils
            juce::juce_audio_processors
            juce::juce_audio_formats
            juce::juce_audio_basics
            juce::juce_dsp
            juce::juce_gui_extra
            juce::juce_core
        PUBLIC
            juce::juce_recommended_config_flags
    )
endfunction()

# ==============================================================================
# Target 1: VictoryVF50 (Stereo VST3 / AU / LV2 / Standalone + CLAP)
# ==============================================================================
juce_add_plugin(VictoryVF50
    COMPANY_NAME                "Victory Audio"
    BUNDLE_ID                   "com.victoryaudio.vf50"
    PLUGIN_NAME                 "Victory VF50"
    IS_SYNTH                    TRUE
    NEEDS_MIDI_INPUT            TRUE
    NEEDS_MIDI_OUTPUT           TRUE
    IS_MIDI_EFFECT              FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    COPY_PLUGIN_AFTER_BUILD     ${VF50_DEPLOY_LOCAL}
    PLUGIN_MANUFACTURER_CODE    VA50
    PLUGIN_CODE                 VF50
    LV2URI                      "https://victoryvf50.org/plugins/victory-vf50"
    FORMATS                     ${VF50_PLUGIN_FORMATS}
    PRODUCT_NAME                "Victory VF50"
)
vf50_configure_plugin_target(VictoryVF50 0 "com.victoryaudio.vf50")

# ==============================================================================
# Target 2: VictoryVF50_Multi (10-Stereo-Bus VST3 / AU / LV2 + CLAP)
# ==============================================================================
juce_add_plugin(VictoryVF50_Multi
    COMPANY_NAME                "Victory Audio"
    BUNDLE_ID                   "com.victoryaudio.vf50.multi"
    PLUGIN_NAME                 "Victory VF50 Multi"
    IS_SYNTH                    TRUE
    NEEDS_MIDI_INPUT            TRUE
    NEEDS_MIDI_OUTPUT           TRUE
    IS_MIDI_EFFECT              FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    COPY_PLUGIN_AFTER_BUILD     ${VF50_DEPLOY_LOCAL}
    PLUGIN_MANUFACTURER_CODE    VA50
    PLUGIN_CODE                 VF5M
    LV2URI                      "https://victoryvf50.org/plugins/victory-vf50-multi"
    FORMATS                     ${VF50_MULTI_PLUGIN_FORMATS}
    PRODUCT_NAME                "Victory VF50 Multi"
)
vf50_configure_plugin_target(VictoryVF50_Multi 1 "com.victoryaudio.vf50.multi")

# ==============================================================================
# Targets 3 & 4: HeadlessTest (Stereo & Multi-Out Verifiers)
# ==============================================================================
juce_add_console_app(VF50_HeadlessTest
    PRODUCT_NAME "VF50_HeadlessTest"
)
vf50_configure_headless_target(VF50_HeadlessTest 0)

juce_add_console_app(VF50_HeadlessTest_Multi
    PRODUCT_NAME "VF50_HeadlessTest_Multi"
)
vf50_configure_headless_target(VF50_HeadlessTest_Multi 1)
