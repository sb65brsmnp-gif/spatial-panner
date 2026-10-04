# The standalone app: JUCE window with the editor (ui/) in a WebView and the
# engine behind it. The editor is built with npm and embedded as one file.

find_program(NPM_EXECUTABLE npm)
if(NOT NPM_EXECUTABLE)
  message(FATAL_ERROR "The app embeds the editor UI, which needs Node.js and npm to build "
                      "(macOS: brew install node). Or configure with -DSP_BUILD_APP=OFF.")
endif()

set(SP_UI_DIR ${CMAKE_SOURCE_DIR}/ui)
set(SP_UI_OUT ${CMAKE_CURRENT_BINARY_DIR}/ui)
file(GLOB_RECURSE SP_UI_SOURCES CONFIGURE_DEPENDS ${SP_UI_DIR}/src/*)
# npm ci only when the lockfile changes; the build whenever a source does.
add_custom_command(
  OUTPUT ${SP_UI_OUT}/npm-ci.stamp
  COMMAND ${CMAKE_COMMAND} -E env PLAYWRIGHT_SKIP_BROWSER_DOWNLOAD=1 ${NPM_EXECUTABLE} ci --no-audit --no-fund
  COMMAND ${CMAKE_COMMAND} -E touch ${SP_UI_OUT}/npm-ci.stamp
  WORKING_DIRECTORY ${SP_UI_DIR}
  DEPENDS ${SP_UI_DIR}/package.json ${SP_UI_DIR}/package-lock.json
  COMMENT "Installing the editor UI's npm packages"
  VERBATIM)
add_custom_command(
  OUTPUT ${SP_UI_OUT}/index.html
  COMMAND ${NPM_EXECUTABLE} run build -- --outDir ${SP_UI_OUT}/dist --emptyOutDir
  COMMAND ${CMAKE_COMMAND} -E copy ${SP_UI_OUT}/dist/index.html ${SP_UI_OUT}/index.html
  WORKING_DIRECTORY ${SP_UI_DIR}
  DEPENDS ${SP_UI_SOURCES} ${SP_UI_DIR}/index.html ${SP_UI_DIR}/vite.config.ts ${SP_UI_DIR}/tsconfig.json ${SP_UI_OUT}/npm-ci.stamp
  COMMENT "Building the editor UI"
  VERBATIM)

juce_add_binary_data(sp_ui_data SOURCES ${SP_UI_OUT}/index.html)

# The plist goes to JUCE as text, on one line, without the XML header.
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/Resources/DocumentTypes.plist SP_DOC_TYPES_PLIST)
string(REGEX REPLACE "<\\?xml[^>]*>|<!DOCTYPE[^>]*>|<!--[^>]*-->" "" SP_DOC_TYPES_PLIST "${SP_DOC_TYPES_PLIST}")
string(REGEX REPLACE "[\r\n]+ *" "" SP_DOC_TYPES_PLIST "${SP_DOC_TYPES_PLIST}")

juce_add_gui_app(SpatialPannerApp
  PRODUCT_NAME "Spatial Panner"
  VERSION ${PROJECT_VERSION}
  COMPANY_NAME "Spatial Panner"
  BUNDLE_ID com.spatialpanner.app
  NEEDS_WEB_BROWSER TRUE
  MICROPHONE_PERMISSION_ENABLED FALSE
  # .spscene files open in the app from the Finder (Resources/DocumentTypes.plist).
  PLIST_TO_MERGE "${SP_DOC_TYPES_PLIST}")

target_sources(SpatialPannerApp PRIVATE
  Source/Main.cpp
  Source/MainComponent.cpp
  Source/Session.cpp
  Source/Bridge.cpp
  Source/AudioLibrary.cpp)
if(APPLE)
  target_sources(SpatialPannerApp PRIVATE Source/MacSupport.mm)
endif()

target_compile_definitions(SpatialPannerApp PRIVATE
  JUCE_WEB_BROWSER=1
  JUCE_USE_CURL=0
  JUCE_USE_MP3AUDIOFORMAT=1
  JUCE_DISPLAY_SPLASH_SCREEN=0
  JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:SpatialPannerApp,JUCE_PRODUCT_NAME>"
  JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:SpatialPannerApp,JUCE_VERSION>"
  SP_APP_VERSION="${PROJECT_VERSION}")

target_link_libraries(SpatialPannerApp PRIVATE
  sp_ui_data
  sp::engine
  saf
  juce::juce_audio_utils
  juce::juce_audio_devices
  juce::juce_audio_formats
  juce::juce_gui_extra
  juce::juce_recommended_config_flags
  juce::juce_recommended_warning_flags)

# The default HRTF goes inside the app (macOS: Contents/Resources; elsewhere
# next to the executable).
if(EXISTS ${SP_DEFAULT_HRTF} OR SP_FETCH_HRTF)
  if(APPLE)
    add_custom_command(TARGET SpatialPannerApp POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_CONTENT_DIR:SpatialPannerApp>/Resources"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SP_DEFAULT_HRTF} "$<TARGET_BUNDLE_CONTENT_DIR:SpatialPannerApp>/Resources/sadie_d1.sofa"
      VERBATIM)
  else()
    add_custom_command(TARGET SpatialPannerApp POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SP_DEFAULT_HRTF} "$<TARGET_FILE_DIR:SpatialPannerApp>/sadie_d1.sofa"
      VERBATIM)
  endif()
endif()

# Tests for the audio session (fake audio device, no window).
if(SP_BUILD_TESTS)
  juce_add_console_app(sp_app_tests PRODUCT_NAME "sp_app_tests")
  target_sources(sp_app_tests PRIVATE tests/test_session.cpp Source/Session.cpp Source/AudioLibrary.cpp)
  target_include_directories(sp_app_tests PRIVATE Source)
  target_compile_definitions(sp_app_tests PRIVATE JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0 JUCE_MODAL_LOOPS_PERMITTED=1)
  target_link_libraries(sp_app_tests PRIVATE sp::engine saf Catch2::Catch2
    juce::juce_audio_devices juce::juce_audio_formats juce::juce_events juce::juce_recommended_config_flags)
  add_test(NAME sp_app_tests COMMAND sp_app_tests)
endif()
