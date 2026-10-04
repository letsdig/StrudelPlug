cmake_minimum_required(VERSION 3.22)
project(StrudelPlug VERSION 1.0.9 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "")

# JUCE 8 checkout expected next to this repo, or pass -DJUCE_DIR=<path>
set(JUCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../JUCE" CACHE PATH "JUCE source dir")
add_subdirectory(${JUCE_DIR} juce)

if(APPLE)
    set(FORMATS VST3 AU)
else()
    set(FORMATS VST3)
endif()

juce_add_plugin(StrudelPlug
    PRODUCT_NAME "StrudelPlug"
    COMPANY_NAME "letsdig"
    BUNDLE_ID "com.letsdig.strudelplug"
    PLUGIN_MANUFACTURER_CODE Ltdg
    PLUGIN_CODE Strd
    FORMATS ${FORMATS}
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT TRUE
    NEEDS_WEB_BROWSER TRUE
    VST3_CATEGORIES Instrument Tools
    AU_MAIN_TYPE kAudioUnitType_MusicEffect
    COPY_PLUGIN_AFTER_BUILD TRUE)

juce_generate_juce_header(StrudelPlug)

target_sources(StrudelPlug PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)

target_compile_definitions(StrudelPlug PRIVATE
    JUCE_WEB_BROWSER=1
    JUCE_USE_CURL=0
    JUCE_STRICT_REFCOUNTEDPOINTER=1
    JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(StrudelPlug PRIVATE
    juce::juce_audio_utils
    juce::juce_gui_extra
    juce::juce_osc
    juce::juce_midi_ci
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)

if(UNIX AND NOT APPLE)
    target_link_libraries(StrudelPlug PRIVATE gobject-2.0 glib-2.0)
endif()
