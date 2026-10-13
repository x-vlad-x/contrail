#!/usr/bin/env bash
# Runs inside the build container: copies into <dist>/lib the shared libraries bin/contrail needs that the
# phone doesn't have. The container has the same packages as FuriOS, so a library the phone has is the right
# one. <phone-libs> lists the phone's libraries (ldconfig -p); without it, everything except the base system
# (glibc, the graphics, GTK and sound stacks) is bundled. Qt plugins come from the phone too, except WebP.
#   bundle-libs.sh <dist> [phone-libs]
set -euo pipefail
DIST=$1
PHONE_LIBS=${2:-}
declare -A on_phone
if [[ -n $PHONE_LIBS && -s $PHONE_LIBS ]]; then
  while read -r name; do on_phone[$name]=1; done < "$PHONE_LIBS"
fi
LIBDIRS=(/usr/lib/aarch64-linux-gnu /lib/aarch64-linux-gnu)
# Libraries taken from the phone's system.
SYSTEM_RE='^(ld-linux-aarch64|libc|libm|libdl|libpthread|librt|libutil|libresolv|libgcc_s|libstdc\+\+|libatomic'
SYSTEM_RE+='|libgtk-4|libadwaita-1|libglib-2|libgobject-2|libgio-2|libgmodule-2|libgthread-2|libpango|libcairo|libharfbuzz'
SYSTEM_RE+='|libfreetype|libfontconfig|libpixman|libgdk_pixbuf|libgraphene|libepoxy|libfribidi|libthai|libdatrie|libpng16'
SYSTEM_RE+='|libjpeg|libtiff|libwebp|libsharpyuv|libxml2|libz|libbz2|liblzma|libzstd|libbrotli|libexpat|libffi|libpcre2'
SYSTEM_RE+='|libmount|libblkid|libselinux|libuuid|libsystemd|libudev|libcap|libdbus-1|libwayland|libxkbcommon|libEGL|libGL'
SYSTEM_RE+='|libGLES|libGLX|libGLdispatch|libgbm|libdrm|libvulkan|libX|libxcb|libICE|libSM|libasound|libpulse|libpipewire'
SYSTEM_RE+='|libssl|libcrypto|libgnutls|libnettle|libhogweed|libgmp|libidn2|libunistring|libtasn1|libp11-kit|libkrb5|libk5crypto'
SYSTEM_RE+='|libgssapi_krb5|libkrb5support|libcom_err|libkeyutils|libsasl2|libicu|libcloudproviders|libappstream|libtinysparql'
SYSTEM_RE+='|libgstreamer|libgst|liborc|libcolord|liblcms2|libcups|libavahi|libjson-glib|librsvg|libglycin|libseccomp|liblz4)\.so'

find_lib() { for d in "${LIBDIRS[@]}"; do [[ -e $d/$1 ]] && { echo "$d/$1"; return; }; done; return 1; }
needed() { aarch64-linux-gnu-readelf -d "$1" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'; }

mkdir -p "$DIST/lib"
declare -A seen
queue=("$DIST/bin/contrail")
# Telegram Desktop's emoji are WebP images: Qt's WebP plugin, which FuriOS doesn't install.
QTPLUG=/usr/lib/aarch64-linux-gnu/qt6/plugins
mkdir -p "$DIST/plugins/imageformats"
cp -L "$QTPLUG/imageformats/libqwebp.so" "$DIST/plugins/imageformats/"
queue+=("$DIST/plugins/imageformats/libqwebp.so")
while [[ ${#queue[@]} -gt 0 ]]; do
  f=${queue[0]}; queue=("${queue[@]:1}")
  for n in $(needed "$f"); do
    [[ -n ${seen[$n]:-} ]] && continue
    seen[$n]=1
    [[ $n =~ $SYSTEM_RE || -n ${on_phone[$n]:-} ]] && continue
    src=$(find_lib "$n") || { echo "  not found: $n" >&2; continue; }
    cp -L "$src" "$DIST/lib/$n"
    queue+=("$DIST/lib/$n")
  done
done
aarch64-linux-gnu-strip --strip-unneeded "$DIST"/lib/*.so* "$DIST"/plugins/*/*.so 2>/dev/null || true
echo "  bundled $(ls "$DIST/lib" | wc -l) libraries ($(du -sh "$DIST/lib" | cut -f1))"
