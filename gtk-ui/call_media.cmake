# Qt-free 1:1 media, built against the same WebRTC and rnnoise as the stable client.
set(CONTRAIL_CALL_DEPS "${ROOT}/deps-${CMAKE_SYSTEM_PROCESSOR}/prefix" CACHE PATH "WebRTC and rnnoise prefix")
set(CONTRAIL_TGCALLS_DIR "${ROOT}/tdesktop/Telegram/ThirdParty/tgcalls" CACHE PATH "tgcalls sources")
if (NOT EXISTS "${CONTRAIL_CALL_DEPS}/lib/libtg_owt.a")
  message(FATAL_ERROR "Call dependencies missing: ${CONTRAIL_CALL_DEPS}; use CONTRAIL_CALLS=OFF to build without media")
endif()
pkg_check_modules(CONTRAIL_MEDIA_DEPS REQUIRED IMPORTED_TARGET
  libavformat libavcodec libswscale libswresample libavutil opus vpx libjpeg openal
  x11 xcomposite xdamage xext xfixes xrender xrandr xtst gio-2.0)
set(TGCALLS ${CONTRAIL_TGCALLS_DIR}/tgcalls)
add_library(contrail_tgcalls STATIC
  ${TGCALLS}/Instance.cpp
  ${TGCALLS}/AudioDeviceHelper.cpp
  ${TGCALLS}/ChannelManager.cpp
  ${TGCALLS}/CodecSelectHelper.cpp
  ${TGCALLS}/CryptoHelper.cpp
  ${TGCALLS}/EncryptedConnection.cpp
  ${TGCALLS}/FakeAudioDeviceModule.cpp
  ${TGCALLS}/FieldTrialsConfig.cpp
  ${TGCALLS}/InstanceImpl.cpp
  ${TGCALLS}/LogSinkImpl.cpp
  ${TGCALLS}/Manager.cpp
  ${TGCALLS}/MediaManager.cpp
  ${TGCALLS}/Message.cpp
  ${TGCALLS}/NetworkManager.cpp
  ${TGCALLS}/SctpDataChannelProviderInterfaceImpl.cpp
  ${TGCALLS}/StaticThreads.cpp
  ${TGCALLS}/TurnCustomizerImpl.cpp
  ${TGCALLS}/VideoCaptureInterface.cpp
  ${TGCALLS}/VideoCaptureInterfaceImpl.cpp
  ${TGCALLS}/utils/gzip.cpp
  ${TGCALLS}/v2/ContentNegotiation.cpp
  ${TGCALLS}/v2/CustomDcSctpSocket.cpp
  ${TGCALLS}/v2/DirectNetworkingImpl.cpp
  ${TGCALLS}/v2/ExternalSignalingConnection.cpp
  ${TGCALLS}/v2/InstanceV2ReferenceImpl.cpp
  ${TGCALLS}/v2/InstanceV2Impl.cpp
  ${TGCALLS}/v2/MtProtoIceTransport.cpp
  ${TGCALLS}/v2/NativeNetworkingImpl.cpp
  ${TGCALLS}/v2/RawTcpSocket.cpp
  ${TGCALLS}/v2/ReflectorPort.cpp
  ${TGCALLS}/v2/ReflectorRelayPortFactory.cpp
  ${TGCALLS}/v2/Signaling.cpp
  ${TGCALLS}/v2/SignalingConnection.cpp
  ${TGCALLS}/v2/SignalingEncryption.cpp
  ${TGCALLS}/v2/SignalingSctpConnection.cpp
  ${TGCALLS}/desktop_capturer/DesktopCaptureSource.cpp
  ${TGCALLS}/desktop_capturer/DesktopCaptureSourceHelper.cpp
  ${TGCALLS}/desktop_capturer/DesktopCaptureSourceManager.cpp
  ${TGCALLS}/group/AVIOContextImpl.cpp
  ${TGCALLS}/group/AudioStreamingPart.cpp
  ${TGCALLS}/group/AudioStreamingPartInternal.cpp
  ${TGCALLS}/group/AudioStreamingPartPersistentDecoder.cpp
  ${TGCALLS}/group/GroupInstanceCustomImpl.cpp
  ${TGCALLS}/group/GroupJoinPayloadInternal.cpp
  ${TGCALLS}/group/GroupNetworkManager.cpp
  ${TGCALLS}/group/StreamingAudioRenderer.cpp
  ${TGCALLS}/group/StreamingMediaContext.cpp
  ${TGCALLS}/group/VideoStreamingPart.cpp
  ${TGCALLS}/platform/tdesktop/DesktopInterface.cpp
  ${TGCALLS}/platform/tdesktop/VideoCapturerInterfaceImpl.cpp
  ${TGCALLS}/platform/tdesktop/VideoCapturerTrackSource.cpp
  ${TGCALLS}/platform/tdesktop/VideoCameraCapturer.cpp
  ${TGCALLS}/third-party/json11.cpp
  )
target_include_directories(contrail_tgcalls PUBLIC ${CONTRAIL_TGCALLS_DIR} ${TGCALLS})
target_include_directories(contrail_tgcalls SYSTEM PUBLIC
  ${CONTRAIL_CALL_DEPS}/include/tg_owt
  ${CONTRAIL_CALL_DEPS}/include/tg_owt/third_party/libyuv/include
  ${CONTRAIL_CALL_DEPS}/include/tg_owt/third_party/abseil-cpp ${CONTRAIL_CALL_DEPS}/include)
target_compile_definitions(contrail_tgcalls PUBLIC
  BWE_TEST_LOGGING_COMPILE_TIME_ENABLE=0 HAVE_WEBRTC_VIDEO NDEBUG RTC_DISABLE_TRACE_EVENTS
  RTC_ENABLE_H265 RTC_ENABLE_VP9 TGCALLS_USE_STD_OPTIONAL WEBRTC_APM_DEBUG_DUMP=0 WEBRTC_APP_TDESKTOP
  WEBRTC_DUMMY_AUDIO_BUILD WEBRTC_ENABLE_PROTOBUF=0 WEBRTC_HAVE_DCSCTP WEBRTC_HAVE_SCTP
  WEBRTC_INCLUDE_INTERNAL_AUDIO_DEVICE WEBRTC_LIBRARY_IMPL WEBRTC_LINUX
  WEBRTC_NON_STATIC_TRACE_EVENT_HANDLERS=1 WEBRTC_POSIX WEBRTC_USE_H264 WEBRTC_USE_PIPEWIRE WEBRTC_USE_X11)
target_compile_options(contrail_tgcalls PRIVATE -Wno-deprecated-declarations -Wno-deprecated -ffunction-sections -fdata-sections)
target_link_libraries(contrail_tgcalls PUBLIC
  ${CONTRAIL_CALL_DEPS}/lib/libtg_owt.a ${CONTRAIL_CALL_DEPS}/lib/librnnoise.a
  PkgConfig::CONTRAIL_MEDIA_DEPS OpenSSL::SSL OpenSSL::Crypto ZLIB::ZLIB Threads::Threads ${CMAKE_DL_LIBS})
find_library(CONTRAIL_OPENH264 NAMES openh264 PATHS ${CONTRAIL_CALL_DEPS}/lib REQUIRED)
target_link_libraries(contrail_tgcalls PUBLIC ${CONTRAIL_OPENH264})
add_library(contrail_call_media STATIC call_media_tgcalls.cpp video_frame.cpp media/openal_adm.cpp)
target_include_directories(contrail_call_media PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(contrail_call_media PUBLIC contrail_tgcalls PkgConfig::CONTRAIL_GTK_DEPS)
target_compile_options(contrail_call_media PRIVATE -Wall -Wno-unused-parameter -Wno-missing-field-initializers
  -Wno-deprecated-declarations -ffunction-sections -fdata-sections)
target_link_libraries(contrail_core PUBLIC contrail_call_media)

# Original call sounds, embedded as MP3 and decoded on demand with the FFmpeg already used by WebRTC.
find_program(CONTRAIL_CALL_PYTHON python3 REQUIRED)
set(CALL_SOUNDS_DIR ${CONTRAIL_TGCALLS_DIR}/../../Resources/sounds)
set(CALL_SOUNDS_HEADER ${GENERATED}/call_sounds.h)
add_custom_command(OUTPUT ${CALL_SOUNDS_HEADER}
  COMMAND ${CONTRAIL_CALL_PYTHON} ${ROOT}/tools/td-call-sounds.py ${CALL_SOUNDS_DIR} ${CALL_SOUNDS_HEADER}
  DEPENDS ${ROOT}/tools/td-call-sounds.py ${CALL_SOUNDS_DIR}/call_incoming.mp3 ${CALL_SOUNDS_DIR}/call_outgoing.mp3
    ${CALL_SOUNDS_DIR}/call_connect.mp3 ${CALL_SOUNDS_DIR}/call_end.mp3 ${CALL_SOUNDS_DIR}/call_busy.mp3
  VERBATIM)
target_sources(contrail_call_media PRIVATE call_tones.cpp ${CALL_SOUNDS_HEADER})
target_include_directories(contrail_call_media PRIVATE ${GENERATED})
