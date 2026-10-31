#!/usr/bin/env bash
# Builds the probes of the TDLib work for the phone, inside the cross container:
#   build-env/run.sh build-env/td-probes.sh tone
#   build-env/run.sh build-env/td-probes.sh call    (needs api-credentials.env and TDLib in .cache/tdlib-study)
# The output goes to build-td-probes/. Until the new build exists they reuse tg_owt and tgcalls from the current
# tree (deps-aarch64, build-aarch64).
set -euo pipefail
cd /src
OUT=build-td-probes
mkdir -p "$OUT"
P=/src/deps-aarch64/prefix
TGCALLS=/src/tdesktop/Telegram/ThirdParty/tgcalls
DEFINES="-DBWE_TEST_LOGGING_COMPILE_TIME_ENABLE=0 -DHAVE_WEBRTC_VIDEO -DNDEBUG -DRTC_DISABLE_TRACE_EVENTS
  -DRTC_ENABLE_H265 -DRTC_ENABLE_VP9 -DTGCALLS_USE_STD_OPTIONAL -DWEBRTC_APM_DEBUG_DUMP=0 -DWEBRTC_APP_TDESKTOP
  -DWEBRTC_DUMMY_AUDIO_BUILD -DWEBRTC_ENABLE_PROTOBUF=0 -DWEBRTC_HAVE_DCSCTP -DWEBRTC_HAVE_SCTP
  -DWEBRTC_INCLUDE_INTERNAL_AUDIO_DEVICE -DWEBRTC_LIBRARY_IMPL -DWEBRTC_LINUX
  -DWEBRTC_NON_STATIC_TRACE_EVENT_HANDLERS=1 -DWEBRTC_POSIX -DWEBRTC_USE_H264 -DWEBRTC_USE_PIPEWIRE -DWEBRTC_USE_X11"
INCLUDES="-Igtk-ui -I$TGCALLS -I$TGCALLS/tgcalls -isystem $P/include/tg_owt
  -isystem $P/include/tg_owt/third_party/libyuv/include -isystem $P/include/tg_owt/third_party/abseil-cpp
  -isystem $P/include"
CXX="aarch64-linux-gnu-g++ -std=gnu++20 -O2 -g0 -Wall -Wno-deprecated-declarations -ffunction-sections -fdata-sections -fPIC"
LIBS="-Wl,--start-group $P/lib/libtg_owt.a $P/lib/librnnoise.a -Wl,--end-group -L$P/lib -lavformat -lavcodec
  -lswscale -lswresample -lavutil -lssl -lcrypto -lz -lopenh264 -lopus -lvpx -ljpeg -lX11 -lXcomposite -lXdamage
  -lXext -lXfixes -lXrender -lXrandr -lXtst -lgio-2.0 -lgobject-2.0 -lglib-2.0 -lopenal -lpthread -ldl -lm -lrt"

compile() { $CXX $DEFINES $INCLUDES "${@:3}" -c "$1" -o "$OUT/$2.o"; }

$CXX $DEFINES $INCLUDES -c gtk-ui/media/openal_adm.cpp -o "$OUT/openal_adm.o"

case "${1:-tone}" in
  tone)
    compile tests/td/tone-probe.cpp tone-probe
    aarch64-linux-gnu-g++ -Wl,--gc-sections -Wl,--as-needed -s -o "$OUT/tone-probe" "$OUT/tone-probe.o" \
      "$OUT/openal_adm.o" $LIBS
    ls -la "$OUT/tone-probe"
    ;;
  teardown)
    compile tests/td/teardown-probe.cpp teardown-probe
    aarch64-linux-gnu-g++ -Wl,--gc-sections -Wl,--as-needed -s -o "$OUT/teardown-probe" "$OUT/teardown-probe.o" \
      "$OUT/openal_adm.o" /src/build-aarch64/Telegram/liblib_tgcalls.a $LIBS
    ls -la "$OUT/teardown-probe"
    ;;
  call)
    # shellcheck disable=SC1091
    source api-credentials.env
    TD=/src/.cache/tdlib-study
    compile tests/td/call-probe.cpp call-probe -I"$TD/td" -I"$TD/build-aarch64" -I"$TD/td/td/generate/auto" \
      -I"$TD/td/tdutils" -I"$TD/build-aarch64/tdutils" -DPROBE_API_ID="$TELEGRAM_API_ID" \
      -DPROBE_API_HASH="\"$TELEGRAM_API_HASH\""
    B=$TD/build-aarch64
    aarch64-linux-gnu-g++ -Wl,--gc-sections -Wl,--as-needed -o "$OUT/call-probe" "$OUT/call-probe.o" \
      "$OUT/openal_adm.o" /src/build-aarch64/Telegram/liblib_tgcalls.a \
      $B/libtdclient.a $B/libtdcore.a $B/libtdapi.a $B/libtdmtproto.a $B/tde2e/libtde2e.a $B/tddb/libtddb.a \
      $B/sqlite/libtdsqlite.a $B/tdnet/libtdnet.a $B/tdactor/libtdactor.a $B/tdutils/libtdutils.a $B/libmemprof.a \
      $LIBS
    # The symbols stay here, the phone gets the stripped file: they resolve the stack the probe prints when it crashes.
    aarch64-linux-gnu-objcopy --only-keep-debug "$OUT/call-probe" "$OUT/call-probe.debug"
    aarch64-linux-gnu-strip "$OUT/call-probe"
    ls -la "$OUT/call-probe"
    ;;
  *) echo "usage: $0 tone|teardown|call" >&2; exit 1 ;;
esac
