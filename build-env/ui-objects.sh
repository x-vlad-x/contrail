#!/usr/bin/env bash
# Compiles only the interface sources (inside the build container): ./build-env/run.sh build-env/ui-objects.sh
cd /src/build-aarch64
targets=$(ninja -t targets all 2>/dev/null | grep -o '^Telegram/CMakeFiles/Telegram.dir/src/gtk-ui/[^:]*\.o')
ninja -k 0 -j32 $targets
