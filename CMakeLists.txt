cmake_minimum_required(VERSION 3.22)
project(Peacock VERSION 1.0.0 LANGUAGES C CXX)
set(JUCE_DIR "$ENV{USERPROFILE}/PitchCanvas/build/_deps/juce-src" CACHE PATH "JUCE source checkout")
add_subdirectory("${JUCE_DIR}" JUCE EXCLUDE_FROM_ALL)

juce_add_binary_data(PeacockBinaryData
    NAMESPACE PeacockData
    SOURCES
        Resources/res_PNG_31010_60x8100.png
        Resources/res_PNG_31011_515x161.png
        Resources/res_PNG_31013_60x120.png
        Resources/res_PNG_31015_60x300.png
        Resources/res_PNG_31018_515x160.png
        Resources/res_PNG_31021_54x18.png
        Resources/res_PNG_31022_48x18.png
        Resources/res_PNG_31023_41x18.png
        Resources/res_PNG_31024_36x18.png
        Resources/res_PNG_31025_53x18.png
        Resources/res_PNG_31026_22x54.png
        Resources/res_PNG_31027_22x44.png
)

juce_add_plugin(Peacock COMPANY_NAME PAPIELGUAPO PLUGIN_MANUFACTURER_CODE Papi
    PLUGIN_CODE Pcv1 FORMATS VST3 PRODUCT_NAME "Peacock" VST3_CATEGORIES Fx Distortion)
target_sources(Peacock PRIVATE Source/PluginProcessor.cpp)
target_compile_features(Peacock PRIVATE cxx_std_20)
target_compile_definitions(Peacock PUBLIC JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0)
target_link_libraries(Peacock PRIVATE PeacockBinaryData juce::juce_audio_utils)

option(PEACOCK_CAPTURE_EDITOR "Build documentation screenshot tool" OFF)
if(PEACOCK_CAPTURE_EDITOR)
    juce_add_console_app(EditorPhoto PRODUCT_NAME EditorPhoto)
    target_sources(EditorPhoto PRIVATE tools/Capture.cpp)
    target_compile_features(EditorPhoto PRIVATE cxx_std_20)
    target_include_directories(EditorPhoto PRIVATE "${JUCE_DIR}/modules")
    target_link_libraries(EditorPhoto PRIVATE Peacock PeacockBinaryData)
endif()
