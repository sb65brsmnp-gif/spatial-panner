# The plugin: AUv2 on macOS (what Logic loads), VST3 elsewhere so the code is
# compiled and tested on Linux CI too. It embeds the same editor UI as the app
# (the sp_ui_data target from app/CMakeLists.txt) and the default HRTF.

if(NOT TARGET sp_ui_data)
  message(FATAL_ERROR "The plugin embeds the editor UI built for the app: configure with SP_BUILD_APP=ON.")
endif()

if(APPLE)
  set(SP_PLUGIN_FORMATS AU)
else()
  set(SP_PLUGIN_FORMATS VST3)
endif()

juce_add_plugin(SpatialPannerPlugin
  PRODUCT_NAME "Spatial Panner"
  COMPANY_NAME "Spatial Panner"
  BUNDLE_ID com.spatialpanner.plugin
  DESCRIPTION "Layers in a 3D scene, heard from a moving listener"
  VERSION ${PROJECT_VERSION}
  PLUGIN_MANUFACTURER_CODE SpPn
  PLUGIN_CODE Spnr
  AU_MAIN_TYPE kAudioUnitType_Effect
  FORMATS ${SP_PLUGIN_FORMATS}
  IS_SYNTH FALSE
  NEEDS_MIDI_INPUT FALSE
  NEEDS_MIDI_OUTPUT FALSE
  IS_MIDI_EFFECT FALSE
  EDITOR_WANTS_KEYBOARD_FOCUS TRUE
  NEEDS_WEB_BROWSER TRUE
  COPY_PLUGIN_AFTER_BUILD FALSE)

set(SP_PLUGIN_SOURCES
  Source/SharedSession.cpp
  Source/ListenerTimeline.cpp
  Source/LayerPathHistory.cpp
  Source/LayerEngine.cpp
  Source/SceneDoc.cpp
  Source/LayerFile.cpp
  Source/PluginProcessor.cpp)

target_sources(SpatialPannerPlugin PRIVATE ${SP_PLUGIN_SOURCES} Source/PluginEditor.cpp)
if(APPLE)
  target_sources(SpatialPannerPlugin PRIVATE Source/WebViewKeys.mm)
endif()

target_compile_definitions(SpatialPannerPlugin PUBLIC
  JUCE_WEB_BROWSER=1
  JUCE_USE_CURL=0
  JUCE_VST3_CAN_REPLACE_VST2=0
  JUCE_DISPLAY_SPLASH_SCREEN=0
  SP_DEFAULT_HRTF="${SP_DEFAULT_HRTF}"
  SP_BUILD_ID="${SP_BUILD_ID}")

target_link_libraries(SpatialPannerPlugin
  PRIVATE
    sp_ui_data
    sp::engine
    saf
    juce::juce_audio_utils
    juce::juce_gui_extra
  PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

# The default HRTF goes inside the bundle (Contents/Resources), then the
# bundle is signed ad hoc: all a plugin built on this Mac needs to load.
if(APPLE)
  add_custom_command(TARGET SpatialPannerPlugin_AU POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_CONTENT_DIR:SpatialPannerPlugin_AU>/Resources"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SP_DEFAULT_HRTF} "$<TARGET_BUNDLE_CONTENT_DIR:SpatialPannerPlugin_AU>/Resources/sadie_d1.sofa"
    COMMAND codesign --force --sign - --timestamp=none "$<TARGET_BUNDLE_DIR:SpatialPannerPlugin_AU>"
    VERBATIM)
else()
  add_custom_command(TARGET SpatialPannerPlugin_VST3 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SP_DEFAULT_HRTF} "$<TARGET_FILE_DIR:SpatialPannerPlugin_VST3>/sadie_d1.sofa"
    VERBATIM)
endif()

# Tests: the processor driven like a host drives it (playhead, blocks, state),
# several instances in one process sharing a session file.
if(SP_BUILD_TESTS)
  juce_add_console_app(sp_plugin_tests PRODUCT_NAME "sp_plugin_tests")
  target_sources(sp_plugin_tests PRIVATE tests/test_plugin.cpp ${SP_PLUGIN_SOURCES})
  target_include_directories(sp_plugin_tests PRIVATE Source)
  target_compile_definitions(sp_plugin_tests PRIVATE
    JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0 JUCE_MODAL_LOOPS_PERMITTED=1
    SP_DEFAULT_HRTF="${SP_DEFAULT_HRTF}"
    SP_SCENES_DIR="${CMAKE_SOURCE_DIR}/scenes")
  target_link_libraries(sp_plugin_tests PRIVATE sp::engine saf Catch2::Catch2
    juce::juce_audio_processors juce::juce_audio_utils juce::juce_recommended_config_flags)
  add_test(NAME sp_plugin_tests COMMAND sp_plugin_tests)
endif()
