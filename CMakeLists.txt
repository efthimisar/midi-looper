cmake_minimum_required(VERSION 3.22)

# macOS: universal (Intel + Apple Silicon), ελάχιστο macOS 10.13. Πρέπει να οριστούν ΠΡΙΝ το project().
set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "Minimum macOS version")
set(CMAKE_OSX_ARCHITECTURES "x86_64;arm64" CACHE STRING "macOS architectures")

project(MidiLooper VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.15
    GIT_SHALLOW ON)
FetchContent_MakeAvailable(JUCE)

set(LOOPER_SOURCES
    Source/LooperEngine.h
    Source/PluginProcessor.h
    Source/PluginProcessor.cpp
    Source/PluginEditor.h
    Source/PluginEditor.cpp)

if (APPLE)
    set(LOOPER_FORMATS VST3 AU)
else()
    set(LOOPER_FORMATS VST3)
endif()

# Φτιάχνει μία παραλλαγή του plugin.
#   is_effect = TRUE  → "MIDI effect" (χωρίς ήχο, μόνο MIDI μέσα/έξω)
#   is_effect = FALSE → "instrument" με σιωπηλή στερεοφωνική έξοδο (για hosts που δεν δέχονται MIDI effects)
function(add_looper_variant target product_name plugin_code is_effect)
    if (is_effect)
        set(effect_flag TRUE)
        set(synth_flag FALSE)
    else()
        set(effect_flag FALSE)
        set(synth_flag TRUE)
    endif()

    juce_add_plugin(${target}
        COMPANY_NAME "HobbyAudio"
        PLUGIN_MANUFACTURER_CODE Hbby
        PLUGIN_CODE ${plugin_code}
        FORMATS ${LOOPER_FORMATS}
        PRODUCT_NAME "${product_name}"
        IS_SYNTH ${synth_flag}
        IS_MIDI_EFFECT ${effect_flag}
        NEEDS_MIDI_INPUT TRUE
        NEEDS_MIDI_OUTPUT TRUE
        COPY_PLUGIN_AFTER_BUILD FALSE)

    target_sources(${target} PRIVATE ${LOOPER_SOURCES})
    target_include_directories(${target} PRIVATE Source)

    target_compile_definitions(${target} PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_DISPLAY_SPLASH_SCREEN=0)

    target_link_libraries(${target}
        PRIVATE
            juce::juce_audio_utils
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_lto_flags
            juce::juce_recommended_warning_flags)
endfunction()

add_looper_variant(MidiLooperFX   "MIDI Looper"      MlpE TRUE)
add_looper_variant(MidiLooperInst "MIDI Looper Inst" MlpI FALSE)
