# Contrail on TDLib: the GTK4 + libadwaita interface, without Qt and without Telegram Desktop.
#   cmake -S gtk-ui -B build-td/<architecture> -DCONTRAIL_TD_DIR=deps-td -DTELEGRAM_API_ID=... -DTELEGRAM_API_HASH=...
# Only the files already moved to TDLib are listed; the others stay in this directory as the model for that work.
cmake_minimum_required(VERSION 3.25)
project(Contrail LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if (NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

set(CONTRAIL_TD_DIR "" CACHE PATH "deps-td from build-env/build-tdlib.sh")
set(TELEGRAM_API_ID "" CACHE STRING "From my.telegram.org")
set(TELEGRAM_API_HASH "" CACHE STRING "From my.telegram.org")
if (NOT CONTRAIL_TD_DIR OR NOT TELEGRAM_API_ID OR NOT TELEGRAM_API_HASH)
  message(FATAL_ERROR "CONTRAIL_TD_DIR, TELEGRAM_API_ID and TELEGRAM_API_HASH are required")
endif()

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)
pkg_check_modules(CONTRAIL_GTK_DEPS REQUIRED IMPORTED_TARGET gtk4 libadwaita-1)
# Photos are decoded with these directly (picture_decode.cpp); GTK links them anyway.
pkg_check_modules(CONTRAIL_IMAGE_DEPS REQUIRED IMPORTED_TARGET libjpeg libpng16)

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(GENERATED ${CMAKE_CURRENT_BINARY_DIR}/generated)

# The stylesheet is compiled in.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/style.css ${ROOT}/VERSION)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/style.css CONTRAIL_CSS)
file(CONFIGURE OUTPUT ${GENERATED}/style_css.h
  CONTENT "#pragma once\ninline constexpr char kStyleCss[] = R\"contrail_css(@CONTRAIL_CSS@)contrail_css\";\n" @ONLY)
file(STRINGS ${ROOT}/VERSION CONTRAIL_VERSION LIMIT_COUNT 1)
file(CONFIGURE OUTPUT ${GENERATED}/contrail_version.h
  CONTENT "#pragma once\ninline constexpr char kContrailVersion[] = \"@CONTRAIL_VERSION@\";\n" @ONLY)
file(CONFIGURE OUTPUT ${GENERATED}/contrail_api.h
  CONTENT "#pragma once\ninline constexpr int kApiId = @TELEGRAM_API_ID@;\ninline constexpr char kApiHash[] = \"@TELEGRAM_API_HASH@\";\n" @ONLY)

# TDLib's libraries, in link order.
if (CMAKE_CROSSCOMPILING)
  set(TD_BUILD ${CONTRAIL_TD_DIR}/aarch64)
else()
  set(TD_BUILD ${CONTRAIL_TD_DIR}/${CMAKE_SYSTEM_PROCESSOR})
endif()
add_library(tdlib INTERFACE)
target_include_directories(tdlib SYSTEM INTERFACE ${CONTRAIL_TD_DIR}/src ${TD_BUILD} ${CONTRAIL_TD_DIR}/src/td/generate/auto
  ${CONTRAIL_TD_DIR}/src/tdutils ${TD_BUILD}/tdutils)
target_link_libraries(tdlib INTERFACE
  ${TD_BUILD}/libtdclient.a ${TD_BUILD}/libtdcore.a ${TD_BUILD}/libtdapi.a ${TD_BUILD}/libtdmtproto.a
  ${TD_BUILD}/tde2e/libtde2e.a ${TD_BUILD}/tddb/libtddb.a ${TD_BUILD}/sqlite/libtdsqlite.a ${TD_BUILD}/tdnet/libtdnet.a
  ${TD_BUILD}/tdactor/libtdactor.a ${TD_BUILD}/tdutils/libtdutils.a ${TD_BUILD}/libmemprof.a
  OpenSSL::SSL OpenSSL::Crypto ZLIB::ZLIB Threads::Threads ${CMAKE_DL_LIBS})

# Everything but the entry point: the tests link it too.
add_library(contrail_core STATIC
  album_layout.cpp
  app.cpp
  chat_list.cpp
  chat_live_locations.cpp
  chat_view.cpp
  debug.cpp
  device_scale.cpp
  dialogs.cpp
  forward.cpp
  live_location.cpp
  live_map.cpp
  local_state.cpp
  location.cpp
  location_ui.cpp
  login.cpp
  main_window.cpp
  map_view.cpp
  memory_trim.cpp
  message.cpp
  photo_viewer.cpp
  picture_decode.cpp
  reactions.cpp
  td/chat_row.cpp
  td/client.cpp
  td/dates.cpp
  td/message_text.cpp
  td/scenario.cpp
  td/session.cpp
  td/store.cpp
  td/strings.cpp
  text.cpp
  topic_list.cpp
  userpics.cpp
  zoom_view.cpp)
# Settings and the app in the background: notifications, the tray icon, memory, the proxy.
target_sources(contrail_core PRIVATE
  background_debug.cpp
  memory_guard.cpp
  network.cpp
  notifications.cpp
  proxy.cpp
  settings.cpp
  tray.cpp)

target_include_directories(contrail_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${GENERATED})
target_link_libraries(contrail_core PUBLIC tdlib PkgConfig::CONTRAIL_GTK_DEPS PkgConfig::CONTRAIL_IMAGE_DEPS)
# Optional: maps for locations (picking a place, following a live location). Without it a location opens in
# the system's map application.
pkg_check_modules(CONTRAIL_SHUMATE IMPORTED_TARGET shumate-1.0)
if (CONTRAIL_SHUMATE_FOUND)
  target_compile_definitions(contrail_core PUBLIC CONTRAIL_SHUMATE)
  target_link_libraries(contrail_core PUBLIC PkgConfig::CONTRAIL_SHUMATE)
endif()
# GTK and libadwaita versions of FuriOS 14: warn about anything newer. The x86_64 test container has older headers,
# which don't know these macros.
if (CMAKE_CROSSCOMPILING)
  target_compile_definitions(contrail_core PUBLIC GLIB_VERSION_MIN_REQUIRED=GLIB_VERSION_2_84
    GLIB_VERSION_MAX_ALLOWED=GLIB_VERSION_2_84 GDK_VERSION_MIN_REQUIRED=GDK_VERSION_4_12
    GDK_VERSION_MAX_ALLOWED=GDK_VERSION_4_22 ADW_VERSION_MIN_REQUIRED=ADW_VERSION_1_6
    ADW_VERSION_MAX_ALLOWED=ADW_VERSION_1_9)
endif()
target_compile_options(contrail_core PUBLIC -Wall -Wno-missing-field-initializers -Wno-unused-parameter
  -ffunction-sections -fdata-sections)

add_executable(contrail main.cpp)
target_link_libraries(contrail PRIVATE contrail_core)
target_link_options(contrail PRIVATE LINKER:--gc-sections LINKER:--as-needed)
# The relative relocations packed (DT_RELR): 2 MB of them read and applied at every start otherwise. glibc 2.36
# reads them; the binary already needs 2.38.
include(CheckLinkerFlag)
check_linker_flag(CXX "LINKER:-z,pack-relative-relocs" CONTRAIL_PACK_RELOCS)
if (CONTRAIL_PACK_RELOCS)
  target_link_options(contrail PRIVATE LINKER:-z,pack-relative-relocs)
endif()

option(CONTRAIL_TESTS "Build the tests of the scripted transport" OFF)
if (CONTRAIL_TESTS)
  add_executable(td-client-test ${ROOT}/tests/td/client-test.cpp)
  target_link_libraries(td-client-test PRIVATE contrail_core)
  add_executable(td-store-test ${ROOT}/tests/td/store-test.cpp)
  target_link_libraries(td-store-test PRIVATE contrail_core)
  add_executable(td-message-text-test ${ROOT}/tests/td/message-text-test.cpp)
  target_link_libraries(td-message-text-test PRIVATE contrail_core)
  add_executable(picture-decode-test ${ROOT}/tests/td/picture-decode-test.cpp)
  target_link_libraries(picture-decode-test PRIVATE contrail_core)
  add_executable(memory-pressure-test ${ROOT}/tests/memory-pressure.cpp)
  target_link_libraries(memory-pressure-test PRIVATE contrail_core)
  # Its checks are asserts: kept in a release build too.
  target_compile_options(memory-pressure-test PRIVATE -UNDEBUG)
  # The viewer's and the player's arithmetic, without a display.
  add_executable(zoom-geometry-test ${ROOT}/tests/zoom-geometry.cpp)
  target_include_directories(zoom-geometry-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_executable(media-progress-test ${ROOT}/tests/media-progress.cpp)
  target_include_directories(media-progress-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_definitions(td-message-text-test PRIVATE
    TD_API_TL="${ROOT}/deps-td/src/td/generate/scheme/td_api.tl")
endif()

# Section media-view: voice messages and music are played and recorded through FFmpeg and OpenAL.
pkg_check_modules(CONTRAIL_PLAYER_DEPS REQUIRED IMPORTED_TARGET libavformat libavcodec libswresample libavutil openal)
target_sources(contrail_core PRIVATE media/audio_player.cpp media/voice_recorder.cpp voice_record_bar.cpp)
target_link_libraries(contrail_core PUBLIC PkgConfig::CONTRAIL_PLAYER_DEPS)
if (CONTRAIL_TESTS)
  add_executable(voice-waveform-test ${ROOT}/tests/td/voice-waveform-test.cpp)
  target_link_libraries(voice-waveform-test PRIVATE contrail_core)
endif()

# Section media-send: files sent from the attach dialog and the clipboard.
pkg_check_modules(CONTRAIL_ATTACH_DEPS REQUIRED IMPORTED_TARGET libswscale)
target_sources(contrail_core PRIVATE attach.cpp attach_media.cpp)
target_link_libraries(contrail_core PUBLIC PkgConfig::CONTRAIL_ATTACH_DEPS)
if (CONTRAIL_TESTS)
  add_executable(paste-test ${ROOT}/tests/paste.cpp)
  target_link_libraries(paste-test PRIVATE contrail_core)
  add_executable(attach-test ${ROOT}/tests/td/attach-test.cpp)
  target_link_libraries(attach-test PRIVATE contrail_core)
endif()

# Section calls: signaling and the scripted media also build without WebRTC.
target_sources(contrail_core PRIVATE call_backend.cpp call_media_scenario.cpp calls.cpp
  audio_route.cpp proximity.cpp call_shell.cpp call_notification.cpp call_lockscreen.cpp)
pkg_check_modules(CONTRAIL_CALL_SHELL REQUIRED IMPORTED_TARGET json-glib-1.0)
target_link_libraries(contrail_core PUBLIC PkgConfig::CONTRAIL_CALL_SHELL)
pkg_check_modules(CONTRAIL_LAYER_SHELL QUIET IMPORTED_TARGET gtk4-layer-shell-0)
if (CONTRAIL_LAYER_SHELL_FOUND)
  target_compile_definitions(contrail_core PRIVATE CONTRAIL_LAYER_SHELL)
  target_link_libraries(contrail_core PUBLIC PkgConfig::CONTRAIL_LAYER_SHELL)
endif()
option(CONTRAIL_CALLS "Build sound and video for 1:1 calls" OFF)
if (NOT CONTRAIL_CALLS)
  target_sources(contrail_core PRIVATE call_media_stub.cpp)
else()
  include(call_media.cmake)
endif()
if (CONTRAIL_TESTS)
  add_executable(td-call-test ${ROOT}/tests/td/call-test.cpp)
  target_link_libraries(td-call-test PRIVATE contrail_core)
endif()

if (CONTRAIL_TESTS AND CONTRAIL_CALLS)
  add_executable(call-media-test ${ROOT}/tests/td/call-media-test.cpp)
  target_include_directories(call-media-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(call-media-test PRIVATE contrail_call_media)
endif()
if (CONTRAIL_TESTS)
  add_executable(audio-route-test ${ROOT}/tests/td/audio-route-test.cpp)
  target_link_libraries(audio-route-test PRIVATE contrail_core)
endif()
if (CONTRAIL_TESTS)
  add_executable(call-shell-test ${ROOT}/tests/td/call-shell-test.cpp)
  target_link_libraries(call-shell-test PRIVATE contrail_core)
endif()
